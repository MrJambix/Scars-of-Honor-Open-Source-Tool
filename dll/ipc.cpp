// ════════════════════════════════════════════════════════════════════════════
// ipc.cpp -- see ipc.h
// ════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include "ipc.h"
#include "log.h"
#include "crash_guard.h"

#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <string>
#include <future>

namespace ipc {

namespace {

constexpr const char* kPipeName = R"(\\.\pipe\ScarsTool)";

struct CmdEntry {
    Handler h;
    bool    onMain = false;
};

std::mutex                                  g_regMu;
std::unordered_map<std::string, CmdEntry>   g_registry;

// Queue of jobs the pipe thread submits to the main thread.
struct Job {
    Handler                  h;
    std::string              args;
    std::promise<std::string> reply;
};
std::mutex                  g_qMu;
std::vector<Job*>           g_queue;

// Pipe server thread state.
std::thread                 g_thread;
std::atomic<bool>           g_quit{false};
HANDLE                      g_wakeEvent = nullptr;   // signals shutdown wake-up

// Helpers ------------------------------------------------------------------

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string EscapeForLine(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if      (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

CmdEntry* Lookup(const std::string& cmd) {
    std::lock_guard<std::mutex> lk(g_regMu);
    auto it = g_registry.find(cmd);
    return it == g_registry.end() ? nullptr : &it->second;
}

// SEH-guarded trampoline.  Contains NO C++ destructible temporaries
// (handler returns via out-param) so MSVC accepts __try.
static unsigned long InvokeGuarded(Handler* h, const std::string* args,
                                   std::string* outReply) {
    __try {
        (*h)(*args, *outReply);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

static std::string CallHandler(Handler& h, const std::string& args,
                               const char* whereTag) {
    std::string reply;
    unsigned long ec = InvokeGuarded(&h, &args, &reply);
    if (ec) {
        char buf[128];
        wsprintfA(buf, "{\"err\":\"SEH 0x%08lX in handler (%s)\"}", ec, whereTag);
        return buf;
    }
    return reply;
}

std::string Dispatch(const std::string& reqLine) {
    std::string line = Trim(reqLine);
    if (line.empty()) return std::string("{\"err\":\"empty request\"}");

    size_t sp = line.find(' ');
    std::string cmd  = (sp == std::string::npos) ? line             : line.substr(0, sp);
    std::string args = (sp == std::string::npos) ? std::string("")  : Trim(line.substr(sp + 1));

    CmdEntry* e = Lookup(cmd);
    if (!e) {
        return std::string("{\"err\":\"unknown command: ") + EscapeForLine(cmd) + "\"}";
    }

    if (!e->onMain) {
        return CallHandler(e->h, args, "pipe-thread");
    }

    // Main-thread dispatch: enqueue + wait on the promise.  Time-out after
    // 5 s so a stalled main thread can't hang the pipe forever.
    Job* j = new Job{ e->h, args, std::promise<std::string>() };
    auto fut = j->reply.get_future();
    {
        std::lock_guard<std::mutex> lk(g_qMu);
        g_queue.push_back(j);
    }
    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        // Job is leaked on timeout (main thread might still process it).
        return "{\"err\":\"main-thread timeout\"}";
    }
    std::string reply = fut.get();
    delete j;
    return reply;
}

// ── Pipe server thread ────────────────────────────────────────────────────
void ServeClient(HANDLE pipe) {
    char buf[4096];
    std::string accum;
    DWORD nr = 0;
    while (!g_quit.load()) {
        BOOL ok = ReadFile(pipe, buf, sizeof(buf) - 1, &nr, nullptr);
        if (!ok || nr == 0) break;
        accum.append(buf, nr);

        // Process complete lines.
        for (;;) {
            size_t nl = accum.find('\n');
            if (nl == std::string::npos) break;
            std::string req = accum.substr(0, nl);
            accum.erase(0, nl + 1);
            std::string reply = Dispatch(req);
            reply.push_back('\n');
            DWORD nw = 0;
            if (!WriteFile(pipe, reply.data(), (DWORD)reply.size(), &nw, nullptr)) {
                return;
            }
        }
    }
}

void PipeThread() {
    LOGI("[ipc] server thread started, pipe=%s", kPipeName);
    while (!g_quit.load()) {
        HANDLE pipe = CreateNamedPipeA(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,                  // single instance, one client at a time
            64 * 1024,
            64 * 1024,
            0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LOGE("[ipc] CreateNamedPipe failed (gle=%lu)", GetLastError());
            Sleep(500);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE
                          : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (g_quit.load()) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            break;
        }
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        LOGI("[ipc] client connected");
        ServeClient(pipe);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        LOGI("[ipc] client disconnected");
    }
    LOGI("[ipc] server thread exiting");
}

} // namespace (anon)

// ── Public API ────────────────────────────────────────────────────────────
void Register(const char* cmd, Handler h, bool runOnMainThread) {
    if (!cmd || !*cmd || !h) return;
    std::lock_guard<std::mutex> lk(g_regMu);
    g_registry[cmd] = CmdEntry{ std::move(h), runOnMainThread };
}

void Pump() {
    // Drain at most a handful per frame to bound time spent here.
    std::vector<Job*> local;
    {
        std::lock_guard<std::mutex> lk(g_qMu);
        if (g_queue.empty()) return;
        local.swap(g_queue);
    }
    for (Job* j : local) {
        std::string reply = CallHandler(j->h, j->args, "main-thread");
        try { j->reply.set_value(std::move(reply)); }
        catch (...) { /* future already abandoned */ }
        // Job memory is freed on the pipe thread side after the future is
        // consumed (or leaked on timeout); don't delete here.
    }
}

void Install() {
    if (g_thread.joinable()) return;
    g_quit.store(false);
    g_wakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    // Built-in ping handler so clients can verify connectivity.
    Register("ping", [](const std::string&, std::string& out) {
        out = "{\"pong\":true}";
    }, /*onMain=*/false);

    g_thread = std::thread(PipeThread);
}

void Shutdown() {
    g_quit.store(true);
    // Kick the blocking ConnectNamedPipe by opening a dummy client.
    HANDLE k = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (k != INVALID_HANDLE_VALUE) CloseHandle(k);
    if (g_thread.joinable()) g_thread.join();
    if (g_wakeEvent) { CloseHandle(g_wakeEvent); g_wakeEvent = nullptr; }
}

} // namespace ipc

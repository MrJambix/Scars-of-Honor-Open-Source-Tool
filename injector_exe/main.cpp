#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <vector>

// Game-specific constants — change these if you fork this for another title.
static const wchar_t* kProcessSubstr = L"scars of honor";   // case-insensitive match
static const wchar_t* kFriendlyName  = L"Scars of Honor";
static const wchar_t* kPayloadDll    = L"ScarsTool.dll";
static const wchar_t* kPayloadModule = L"scarstool";        // case-insensitive substring
static const wchar_t* kIl2cppModule  = L"gameassembly";     // IL2CPP runtime

// ═══════════════════════════════════════════════════════════════════════════════
// Console helpers
// ═══════════════════════════════════════════════════════════════════════════════

static HANDLE g_hConsole = INVALID_HANDLE_VALUE;

static void SetColor(WORD attr) {
    SetConsoleTextAttribute(g_hConsole, attr);
}

static void Info(const char* fmt, ...) {
    SetColor(11); printf("  [*] "); SetColor(7);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static void Ok(const char* fmt, ...) {
    SetColor(10); printf("  [+] "); SetColor(7);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static void Fail(const char* fmt, ...) {
    SetColor(12); printf("  [-] "); SetColor(7);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static void Warn(const char* fmt, ...) {
    SetColor(14); printf("  [!] "); SetColor(7);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

static void PrintBanner() {
    SetColor(13);
    printf("\n");
    printf("   ######   ######     ###    ########   ######  \n");
    printf("  ##    ## ##    ##   ## ##   ##     ## ##    ## \n");
    printf("  ##       ##        ##   ##  ##     ## ##       \n");
    printf("   ######  ##       ##     ## ########   ######  \n");
    printf("        ## ##       ######### ##   ##         ## \n");
    printf("  ##    ## ##    ## ##     ## ##    ##  ##    ## \n");
    printf("   ######   ######  ##     ## ##     ##  ######  \n");
    SetColor(8);
    printf("  =========================================================================\n");
    SetColor(11);
    printf("                    ScarsTool Injector  v1.0\n");
    SetColor(8);
    printf("  =========================================================================\n\n");
    SetColor(7);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Process / module helpers
// ═══════════════════════════════════════════════════════════════════════════════

static std::vector<DWORD> FindAllProcesses(const wchar_t* substr) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t lower[MAX_PATH]{};
            for (int i = 0; pe.szExeFile[i] && i < MAX_PATH - 1; i++)
                lower[i] = towlower(pe.szExeFile[i]);
            if (wcsstr(lower, substr)) {
                pids.push_back(pe.th32ProcessID);
                SetColor(8); printf("      ");
                SetColor(7); wprintf(L"%s", pe.szExeFile);
                SetColor(8); wprintf(L"  (PID %u)\n", pe.th32ProcessID);
                SetColor(7);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static bool HasModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;

    if (Module32FirstW(snap, &me)) {
        do {
            wchar_t lower[MAX_PATH]{};
            for (int i = 0; me.szModule[i] && i < MAX_PATH - 1; i++)
                lower[i] = towlower(me.szModule[i]);
            if (wcsstr(lower, name)) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Injection — LoadLibraryW via CreateRemoteThread
// ═══════════════════════════════════════════════════════════════════════════════

static bool Inject(DWORD pid, const wchar_t* dllPath) {
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        hProc = OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
            FALSE, pid);
    }
    if (!hProc) {
        Fail("PID %u: OpenProcess failed (error %u)", pid, GetLastError());
        return false;
    }

    size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProc, nullptr, pathBytes,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        Fail("PID %u: VirtualAllocEx failed", pid);
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteMem, dllPath, pathBytes, nullptr)) {
        Fail("PID %u: WriteProcessMemory failed", pid);
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW"));
    if (!loadLib) {
        Fail("PID %u: GetProcAddress(LoadLibraryW) failed", pid);
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, remoteMem, 0, nullptr);
    if (!hThread) {
        Fail("PID %u: CreateRemoteThread failed (error %u)", pid, GetLastError());
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    DWORD waitResult = WaitForSingleObject(hThread, 15000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProc);

    if (waitResult == WAIT_TIMEOUT) {
        Warn("PID %u: Remote thread timed out (15s) - DLL may still be loading", pid);
        return true;
    }

    if (exitCode == 0) {
        Fail("PID %u: LoadLibraryW returned NULL - DLL failed to load in target", pid);
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DLL path resolution
// ═══════════════════════════════════════════════════════════════════════════════

static bool GetDllPath(wchar_t* out, DWORD maxChars) {
    if (!GetModuleFileNameW(nullptr, out, maxChars)) return false;
    wchar_t* last = wcsrchr(out, L'\\');
    if (last) *(last + 1) = L'\0';
    wcscat_s(out, maxChars, kPayloadDll);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Wait for IL2CPP and inject a single PID
// ═══════════════════════════════════════════════════════════════════════════════

static bool WaitAndInject(DWORD pid, const wchar_t* dllPath) {
    if (pid == GetCurrentProcessId()) return false;

    if (HasModule(pid, kPayloadModule)) {
        Info("PID %u: Already injected, skipping", pid);
        return true;
    }

    Info("PID %u: Waiting for GameAssembly.dll...", pid);

    bool foundGA = false;
    for (int i = 0; i < 30; i++) {
        if (HasModule(pid, kIl2cppModule)) { foundGA = true; break; }
        Sleep(1000);
        if (i > 0 && i % 5 == 0) {
            SetColor(8); printf("      ... %ds\n", i); SetColor(7);
        }
    }
    if (!foundGA) {
        Warn("PID %u: No GameAssembly.dll after 30s (launcher process?) - skipping", pid);
        return false;
    }

    Ok("PID %u: IL2CPP detected, waiting for initialization...", pid);
    Sleep(3000);

    if (HasModule(pid, kPayloadModule)) {
        Info("PID %u: Already injected (raced), skipping", pid);
        return true;
    }

    Info("PID %u: Injecting %ls...", pid, kPayloadDll);
    if (Inject(pid, dllPath)) {
        Sleep(500);
        if (HasModule(pid, kPayloadModule)) {
            Ok("PID %u: Injected and verified!", pid);
        } else {
            Ok("PID %u: Injection call succeeded (module not yet visible)", pid);
        }
        return true;
    }

    Fail("PID %u: Injection failed", pid);
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Entry point
// ═══════════════════════════════════════════════════════════════════════════════

int main() {
    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTitleA("ScarsTool Injector v1.0");

    DWORD consoleMode = 0;
    GetConsoleMode(g_hConsole, &consoleMode);
    SetConsoleMode(g_hConsole, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    PrintBanner();

    // ── 1. Resolve DLL path ──────────────────────────────────────────────
    wchar_t dllPath[MAX_PATH]{};
    if (!GetDllPath(dllPath, MAX_PATH) ||
        GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        Fail("%ls not found next to injector EXE", kPayloadDll);
        printf("\n  Press Enter to exit...\n");
        getchar();
        return 1;
    }
    SetColor(8); printf("      "); SetColor(7);
    wprintf(L"DLL: %s\n", dllPath);
    Ok("DLL located");
    printf("\n");

    // ── 2. Scan for game processes ───────────────────────────────────────
    Info("Scanning for %ls processes...", kFriendlyName);
    std::vector<DWORD> pids;
    for (int i = 0; i < 120; i++) {
        pids = FindAllProcesses(kProcessSubstr);
        if (!pids.empty()) break;
        if (i == 0) Info("Waiting for game to launch (up to 2 min)...");
        Sleep(1000);
        if (i > 0 && i % 10 == 0) {
            SetColor(8); printf("      ... %ds\n", i); SetColor(7);
        }
    }
    if (pids.empty()) {
        Fail("No game processes found after 2 minutes");
        printf("\n  Press Enter to exit...\n");
        getchar();
        return 1;
    }
    Ok("%d game process(es) found", (int)pids.size());
    printf("\n");

    // ── 3. Inject each PID ───────────────────────────────────────────────
    int successCount = 0;
    int totalTargets = 0;

    for (DWORD pid : pids) {
        totalTargets++;
        if (WaitAndInject(pid, dllPath))
            successCount++;
        printf("\n");
    }

    // ── 4. Summary ───────────────────────────────────────────────────────
    SetColor(8);
    printf("  =========================================================================\n");
    if (successCount == totalTargets) {
        SetColor(10);
        printf("    %d/%d injected successfully!\n", successCount, totalTargets);
    } else if (successCount > 0) {
        SetColor(14);
        printf("    %d/%d injected (some failed - check output above)\n",
               successCount, totalTargets);
    } else {
        SetColor(12);
        printf("    0/%d injected - all attempts failed\n", totalTargets);
    }
    SetColor(8);
    printf("  =========================================================================\n");
    SetColor(7);

    printf("\n  Closing in 3 seconds...\n");
    Sleep(3000);
    return (successCount > 0) ? 0 : 1;
}

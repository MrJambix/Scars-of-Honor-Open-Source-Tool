// ═══════════════════════════════════════════════════════════════════════════════
// ScarsTool.dll  -  Payload injected into "Scars of Honor.exe" (Unity IL2CPP).
//
// This is a starter stub.  Once injected, it spawns a console window, finds the
// IL2CPP runtime (GameAssembly.dll) inside the host process, and waits.  Add
// your hooks, scanners, IPC pipes, etc. in PayloadThread().
// ═══════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <string>

#include "il2cpp_api.h"
#include "sdk_dumper.h"
#include "renderer.h"
#include "overlay.h"
#include "log.h"
#include "game_api.h"

#pragma comment(lib, "psapi.lib")

static HMODULE g_hSelf      = nullptr;
static HMODULE g_hGameAsm   = nullptr;
static HMODULE g_hUnityPlayer = nullptr;
static FILE*   g_pConOut    = nullptr;
static FILE*   g_pConErr    = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Console
// ─────────────────────────────────────────────────────────────────────────────

static void OpenConsole() {
    if (!AllocConsole()) return;
    freopen_s(&g_pConOut, "CONOUT$", "w", stdout);
    freopen_s(&g_pConErr, "CONOUT$", "w", stderr);
    SetConsoleTitleA("ScarsTool  -  Payload Console");

    // Unbuffered output - critical for crash diagnosis.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

static void CloseConsoleSafely() {
    if (g_pConOut) { fclose(g_pConOut); g_pConOut = nullptr; }
    if (g_pConErr) { fclose(g_pConErr); g_pConErr = nullptr; }
    FreeConsole();
}

// ─────────────────────────────────────────────────────────────────────────────
// Module helpers
// ─────────────────────────────────────────────────────────────────────────────

static HMODULE WaitForModule(const wchar_t* name, int timeoutSec) {
    for (int i = 0; i < timeoutSec * 10; i++) {
        HMODULE h = GetModuleHandleW(name);
        if (h) return h;
        Sleep(100);
    }
    return nullptr;
}

static void PrintModuleInfo(const char* label, HMODULE h) {
    if (!h) {
        printf("  [-] %-16s  not loaded\n", label);
        return;
    }
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi));
    printf("  [+] %-16s  base=0x%p  size=0x%X\n",
           label, mi.lpBaseOfDll, mi.SizeOfImage);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main payload thread
// ─────────────────────────────────────────────────────────────────────────────

static DWORD WINAPI PayloadThread(LPVOID) {
    OpenConsole();
    logx::Init();
    LOGI("payload boot, PID=%u", GetCurrentProcessId());    printf("\n");
    printf("  ============================================================\n");
    printf("    ScarsTool payload v1.0  -  loaded into PID %u\n", GetCurrentProcessId());
    printf("  ============================================================\n\n");

    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    printf("  Host EXE : %s\n\n", exePath);

    printf("  Waiting for IL2CPP runtime...\n");
    g_hGameAsm    = WaitForModule(L"GameAssembly.dll", 30);
    g_hUnityPlayer = GetModuleHandleW(L"UnityPlayer.dll");

    PrintModuleInfo("GameAssembly",  g_hGameAsm);
    PrintModuleInfo("UnityPlayer",   g_hUnityPlayer);
    PrintModuleInfo("ScarsTool.dll", g_hSelf);
    printf("\n");

    if (!g_hGameAsm) {
        printf("  [!] GameAssembly.dll never appeared - aborting payload.\n");
        return 1;
    }

    // ── Resolve IL2CPP API ────────────────────────────────────────────
    if (!il2cpp::GetApi().Resolve(g_hGameAsm)) {
        printf("  [!] Some il2cpp_* exports missing - dumper may be partial.\n");
    }
    if (il2cpp::GetApi().IsReady()) {
        printf("  [+] IL2CPP API resolved.\n");
    } else {
        printf("  [-] IL2CPP API NOT ready.\n");
    }

    // Wait a few seconds for the runtime to finish initializing assemblies.
    printf("  [*] Waiting 5s for IL2CPP runtime to settle...\n");
    Sleep(5000);

    // Resolve the central game-API registry now that IL2CPP is up.
    gameapi::Resolve();

    printf("\n  [*] INSERT  in the game window to toggle the dev overlay.\n");
    printf("  [*] F8      to dump the SDK (also available from the overlay).\n");
    printf("  [*] Close this console to detach the tool.\n\n");

    // ── Install renderer + overlay ───────────────────────────────────
    overlay::Init();
    if (renderer::Install()) {
        renderer::SetFrameCallback([] { overlay::Render(); });
        LOGI("overlay armed (press INSERT in-game)");
        printf("  [+] Overlay armed.  Press INSERT in-game.\n");
    } else {
        LOGE("overlay install failed");
        printf("  [-] Overlay install failed; F8 dump still works.\n");
    }

    // Idle loop - F8 triggers another dump.
    bool prevDown = false;
    while (true) {
        bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (down && !prevDown) {
            std::wstring outDir;
            sdk_dumper::Stats st{};
            printf("  [*] F8 pressed - re-dumping SDK...\n");
            if (sdk_dumper::DumpAllAuto(outDir, st)) {
                wprintf(L"  [+] SDK dumped to: %s\n", outDir.c_str());
            } else {
                printf("  [-] SDK dump failed.\n");
            }
        }
        prevDown = down;
        Sleep(50);
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// DllMain
// ─────────────────────────────────────────────────────────────────────────────

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
        if (HANDLE t = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
        break;
    case DLL_PROCESS_DETACH:
        renderer::Uninstall();
        overlay::Shutdown();
        CloseConsoleSafely();
        break;
    }
    return TRUE;
}

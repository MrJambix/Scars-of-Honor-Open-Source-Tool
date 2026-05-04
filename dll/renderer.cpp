// ════════════════════════════════════════════════════════════════════════════
// renderer.cpp  -  DX11 swapchain VMT hook + ImGui plumbing.
// ════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <atomic>

#include "renderer.h"
#include "crash_guard.h"
#include "vendor/minhook/include/MinHook.h"
#include "vendor/imgui/imgui.h"
#include "vendor/imgui/backends/imgui_impl_dx11.h"
#include "vendor/imgui/backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace renderer {

// ─── State ──────────────────────────────────────────────────────────────────
using Pfn_Present       = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Pfn_ResizeBuffers = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Pfn_WndProc       = LRESULT (CALLBACK*)(HWND, UINT, WPARAM, LPARAM);

static Pfn_Present       g_origPresent       = nullptr;
static Pfn_ResizeBuffers g_origResizeBuffers = nullptr;
static Pfn_WndProc       g_origWndProc       = nullptr;

static ID3D11Device*           g_dev      = nullptr;
static ID3D11DeviceContext*    g_ctx      = nullptr;
static ID3D11RenderTargetView* g_rtv      = nullptr;
static HWND                    g_hWnd     = nullptr;
static std::atomic<bool>       g_initOK   { false };
static std::atomic<bool>       g_visible  { true };
static FrameCallback           g_frameCb;
static FrameCallback           g_tickCb;
static int                     g_vpW = 0, g_vpH = 0;

// ─── Helpers ────────────────────────────────────────────────────────────────
static void CreateRTV(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);
        g_vpW = (int)d.Width;
        g_vpH = (int)d.Height;
        back->Release();
    }
}

static void ReleaseRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// ─── Hooks ──────────────────────────────────────────────────────────────────
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_INSERT) {
        bool nowVisible = !g_visible.load();
        g_visible = nowVisible;
        // When opening, free the cursor so the user can actually click.
        if (nowVisible) {
            ClipCursor(nullptr);
            while (ShowCursor(TRUE) < 0) {}
        }
        return 0;
    }

    if (g_initOK.load() && g_visible.load()) {
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp);
        ImGuiIO& io = ImGui::GetIO();

        // Only steal input the user is actually directing at the overlay.
        // Anything ImGui doesn't want goes through to the game so the player
        // can keep playing (camera, movement, hotkeys) while the overlay is
        // visible.
        switch (msg) {
            // Mouse buttons / wheel: swallow only when the cursor is over
            // an ImGui window OR a widget is being actively dragged.
            case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                if (io.WantCaptureMouse) return 0;
                break;

            // Plain mouse motion: never swallow.  ImGui reads it from
            // GetCursorPos() in NewFrame, the game uses it for its own UI
            // hover.  Letting it through fixes "camera frozen" while overlay
            // is up.
            case WM_MOUSEMOVE: case WM_NCMOUSEMOVE:
                break;

            // Keyboard: only steal it when ImGui has focus on a text box,
            // or its nav system is currently capturing.  This lets WASD /
            // hotkeys reach the game.
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                if (io.WantTextInput) return 0;
                break;
            case WM_CHAR: case WM_DEADCHAR:
            case WM_SYSCHAR: case WM_SYSDEADCHAR:
                if (io.WantTextInput) return 0;
                break;

            // Raw input — Unity reads camera-look delta from this.  Only
            // block it when ImGui actively wants the mouse (e.g. dragging
            // a slider) so look/aim keeps working everywhere else.
            case WM_INPUT:
                if (io.WantCaptureMouse) return 0;
                break;

            // Cursor: force a visible arrow only when over ImGui; otherwise
            // let the game restore its own (invisible) cursor for camera.
            case WM_SETCURSOR: {
                if (io.WantCaptureMouse) {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                    return TRUE;
                }
                break;
            }
        }
    }
    return CallWindowProcW(g_origWndProc, hWnd, msg, wp, lp);
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* sc, UINT bufferCount,
                                                     UINT width, UINT height,
                                                     DXGI_FORMAT format, UINT flags) {
    ReleaseRTV();
    HRESULT hr = g_origResizeBuffers(sc, bufferCount, width, height, format, flags);
    if (SUCCEEDED(hr) && g_dev) CreateRTV(sc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    if (!g_initOK.load()) {
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) && g_dev) {
            g_dev->GetImmediateContext(&g_ctx);

            DXGI_SWAP_CHAIN_DESC sd{};
            sc->GetDesc(&sd);
            g_hWnd = sd.OutputWindow;

            CreateRTV(sc);

            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.MouseDrawCursor = true; // draw our own cursor; game hides OS one
            ImGui::StyleColorsDark();

            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX11_Init(g_dev, g_ctx);

            g_origWndProc = (Pfn_WndProc)SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC,
                                                           (LONG_PTR)HookedWndProc);
            g_initOK = true;
            printf("  [renderer] DX11 hook live: hwnd=%p  rtv=%p  size=%dx%d\n",
                   g_hWnd, g_rtv, g_vpW, g_vpH);
        }
    }

    if (g_initOK.load() && g_visible.load() && g_rtv) {
        // Unity re-clips the cursor every frame; undo it while overlay is
        // up so the user can move into our windows.  Input pass-through is
        // handled in HookedWndProc -- only ImGui-targeted clicks/keys are
        // swallowed there, so the game still gets WASD / camera-look.
        ClipCursor(nullptr);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_frameCb) {
            __try { g_frameCb(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                crash_guard::NotifySwallowed("renderer.frameCb", GetExceptionCode());
            }
        }

        ImGui::Render();
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // Background tick: ALWAYS runs (even when overlay is hidden via INSERT)
    // so feature state work keeps happening when the user wants the UI
    // out of the way.  No ImGui frame is open here.
    if (g_initOK.load() && g_tickCb) {
        __try { g_tickCb(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            crash_guard::NotifySwallowed("renderer.tickCb", GetExceptionCode());
        }
    }

    return g_origPresent(sc, syncInterval, flags);
}

// ─── Bootstrap ──────────────────────────────────────────────────────────────
// Create a dummy swapchain to read the vtable for Present/ResizeBuffers.
static bool GetSwapchainVTable(void** outPresent, void** outResize) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ScarsToolDummyWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = hwnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    IDXGISwapChain*      sc  = nullptr;
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr) || !sc) {
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        printf("  [renderer] dummy D3D11CreateDeviceAndSwapChain failed (0x%08X)\n", hr);
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(sc);
    *outPresent = vtable[8];   // IDXGISwapChain::Present
    *outResize  = vtable[13];  // IDXGISwapChain::ResizeBuffers

    sc->Release(); ctx->Release(); dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return true;
}

bool Install() {
    if (MH_Initialize() != MH_OK) {
        printf("  [renderer] MH_Initialize failed.\n");
        return false;
    }
    void* presentTarget = nullptr;
    void* resizeTarget  = nullptr;
    if (!GetSwapchainVTable(&presentTarget, &resizeTarget)) return false;

    if (MH_CreateHook(presentTarget, &HookedPresent, (LPVOID*)&g_origPresent) != MH_OK) {
        printf("  [renderer] hook Present failed.\n"); return false;
    }
    if (MH_CreateHook(resizeTarget, &HookedResizeBuffers, (LPVOID*)&g_origResizeBuffers) != MH_OK) {
        printf("  [renderer] hook ResizeBuffers failed.\n"); return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        printf("  [renderer] MH_EnableHook failed.\n"); return false;
    }
    printf("  [renderer] Present=%p  ResizeBuffers=%p hooked.\n", presentTarget, resizeTarget);
    return true;
}

void Uninstall() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    if (g_hWnd && g_origWndProc) {
        SetWindowLongPtrW(g_hWnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        g_origWndProc = nullptr;
    }
    if (g_initOK.load()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    ReleaseRTV();
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    g_initOK = false;
    g_frameCb = {};
    g_tickCb  = {};
}

void SetFrameCallback(FrameCallback cb) { g_frameCb = std::move(cb); }
void SetTickCallback(FrameCallback cb)  { g_tickCb  = std::move(cb); }
bool IsVisible()                        { return g_visible.load(); }
void SetVisible(bool v)                 { g_visible = v; }
void GetViewportSize(int& w, int& h)    { w = g_vpW; h = g_vpH; }

} // namespace renderer

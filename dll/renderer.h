// ════════════════════════════════════════════════════════════════════════════
// renderer.h  -  DX11 swapchain hook + ImGui initialization for the in-game
// overlay.  Toggle visibility with INSERT.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include <functional>

namespace renderer {

// Install the DX11 hook.  Safe to call once IL2CPP/Unity has initialised
// (i.e. UnityPlayer.dll is loaded and the swapchain exists).
bool Install();

// Tear down hooks and release all D3D/ImGui resources.
void Uninstall();

// Replace the per-frame UI callback.  Called inside Present() with a valid
// ImGui frame already begun.  Cleared by Uninstall().
using FrameCallback = std::function<void()>;
void SetFrameCallback(FrameCallback cb);

// Background tick callback.  Always called inside Present() regardless of
// overlay visibility, OUTSIDE the ImGui frame.  Use for state work that has
// to keep running while the user has the UI hidden (auto-loot, refills, ...).
void SetTickCallback(FrameCallback cb);

// Whether the overlay is currently visible (INSERT toggles).
bool IsVisible();
void SetVisible(bool v);

// Window size of the hooked swapchain (0,0 until first Present).
void GetViewportSize(int& w, int& h);

} // namespace renderer

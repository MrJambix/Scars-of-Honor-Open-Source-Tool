// ════════════════════════════════════════════════════════════════════════════
// overlay.h
// ════════════════════════════════════════════════════════════════════════════
#pragma once

namespace overlay {
void Init();
void Render();   // called inside renderer per-frame callback (UI visible only)
void Tick();     // called every frame, regardless of UI visibility
void Shutdown();
} // namespace overlay

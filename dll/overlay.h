// ════════════════════════════════════════════════════════════════════════════
// overlay.h
// ════════════════════════════════════════════════════════════════════════════
#pragma once

namespace overlay {
void Init();
void Render();   // called inside renderer per-frame callback
void Shutdown();
} // namespace overlay

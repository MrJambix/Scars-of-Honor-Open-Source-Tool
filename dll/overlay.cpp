// ════════════════════════════════════════════════════════════════════════════
// overlay.cpp  -  Thin host that:
//   * runs the per-frame pipeline tick + world rendering, and
//   * draws the dev window with a sidebar listing every Feature grouped by
//     category, and the selected feature's UI body on the right.
//
//   All actual feature logic lives in features.cpp.
// ════════════════════════════════════════════════════════════════════════════
#include "overlay.h"
#include "renderer.h"
#include "il2cpp_helpers.h"
#include "pipeline.h"
#include "sdk_dumper.h"
#include "vendor/imgui/imgui.h"

#include <map>
#include <string>

namespace overlay {

static int g_selected = 0;

void Init() {
    il2cpp_helpers::Init();
    pipeline::InitAll();
}

void Shutdown() {
    pipeline::ShutdownAll();
}

static void DrawSidebar(const std::vector<pipeline::Feature*>& feats) {
    std::map<int, std::vector<int>> byCat; // ordered by enum
    for (int i = 0; i < (int)feats.size(); i++)
        byCat[(int)feats[i]->Cat()].push_back(i);

    ImGui::BeginChild("##sidebar", ImVec2(180, 0), true);
    for (auto& [cat, idxs] : byCat) {
        ImGui::TextDisabled("%s", pipeline::CategoryName((pipeline::Category)cat));
        for (int i : idxs) {
            auto* f = feats[i];
            ImGui::PushID(i);
            bool en = f->Enabled();
            if (ImGui::Checkbox("##en", &en)) f->SetEnabled(en);
            ImGui::SameLine();
            if (ImGui::Selectable(f->Name(), g_selected == i)) g_selected = i;
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Tools");
    if (ImGui::Button("Dump SDK", ImVec2(-1, 0))) {
        std::wstring out;
        sdk_dumper::Stats st{};
        sdk_dumper::DumpAllAuto(out, st);
    }
    ImGui::TextDisabled("FPS %.0f", ImGui::GetIO().Framerate);
    int vw, vh; renderer::GetViewportSize(vw, vh);
    ImGui::TextDisabled("%dx%d", vw, vh);
    ImGui::EndChild();
}

void Tick() {
    // Always-on: feature state work (auto-loot pulses, mana refill, ...).
    pipeline::TickAll();
}

void Render() {
    // World overlays are part of the visual UI -- only when the dev
    // overlay is visible do we draw the foreground stuff.
    pipeline::RenderWorldAll();

    if (!renderer::IsVisible()) return;

    ImGui::SetNextWindowSize(ImVec2(740, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ScarsTool")) { ImGui::End(); return; }

    const auto& feats = pipeline::All();
    if (feats.empty()) {
        ImGui::TextDisabled("No features registered.");
        ImGui::End();
        return;
    }

    DrawSidebar(feats);
    ImGui::SameLine();

    ImGui::BeginChild("##body", ImVec2(0, 0), true);
    if (g_selected < 0 || g_selected >= (int)feats.size()) g_selected = 0;
    auto* f = feats[g_selected];
    ImGui::Text("%s", f->Name());
    ImGui::TextDisabled("Category: %s", pipeline::CategoryName(f->Cat()));
    ImGui::Separator();
    pipeline::RenderUITab(f);
    ImGui::EndChild();

    ImGui::End();
}

} // namespace overlay

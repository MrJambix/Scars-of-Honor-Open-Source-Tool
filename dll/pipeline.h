// ════════════════════════════════════════════════════════════════════════════
// pipeline.h  -  Feature pipeline framework.
//
//   A "pipeline" is a self-contained game feature (mining helper, ESP, player
//   tweaks, ...).  Each pipeline has a lifecycle hook system:
//
//     OnInit()      once, after IL2CPP is up
//     OnTick()      every frame, regardless of UI visibility (state work,
//                   auto-actions, sampling, etc.)
//     OnRenderWorld() draw on the foreground draw list (always-on overlays)
//     OnRenderUI()  draw inside the ImGui dev window's tab body
//     OnShutdown()  once, on detach
//
//   Pipelines self-register at construction time via a static helper.  The
//   overlay just iterates over the registry — adding a new feature means
//   adding one .cpp file with a static instance, no edits elsewhere.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include <vector>
#include <string>

namespace pipeline {

enum class Category {
    Combat,
    Movement,
    Visual,
    MiniGame,
    Debug,
};

const char* CategoryName(Category c);

class Feature {
public:
    Feature(const char* name, Category cat, bool enabledByDefault = false);
    virtual ~Feature() = default;

    const char* Name() const     { return m_name.c_str(); }
    Category    Cat()  const     { return m_cat; }
    bool        Enabled() const  { return m_enabled; }
    void        SetEnabled(bool v) { m_enabled = v; }
    bool*       EnabledPtr()     { return &m_enabled; }

    virtual void OnInit()         {}
    virtual void OnTick()         {}
    virtual void OnRenderWorld()  {}
    virtual void OnRenderUI()     {}
    virtual void OnShutdown()     {}

private:
    std::string m_name;
    Category    m_cat;
    bool        m_enabled;
};

// Registry
const std::vector<Feature*>& All();
void                         Register(Feature* f);

// Lifecycle dispatchers (called from overlay/dllmain)
void InitAll();
void TickAll();             // every frame
void RenderWorldAll();      // every frame, foreground draw list
void RenderUITab(Feature* f);   // body of one tab
void ShutdownAll();

} // namespace pipeline

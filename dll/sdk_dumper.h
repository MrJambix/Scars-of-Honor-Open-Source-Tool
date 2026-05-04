// ════════════════════════════════════════════════════════════════════════════
// sdk_dumper.h  -  Walks the entire IL2CPP runtime and writes:
//   <out>\dump.txt        human-readable, namespace.class.field/method
//   <out>\dump.json       structured (one JSON document)
//   <out>\dump.cs         C#-style pseudo-source (per assembly file optional)
//   <out>\layout.h        C++ struct layouts with field offsets
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include <string>

namespace sdk_dumper {

struct Stats {
    int assemblies = 0;
    int images    = 0;
    int classes   = 0;
    int fields    = 0;
    int methods   = 0;
    int properties = 0;
    int errors    = 0;
};

// Dumps everything to <outDir>.  Creates the directory if needed.
// Returns true on success.  Stats are filled in either way.
bool DumpAll(const std::wstring& outDir, Stats& stats);

// Convenience: dump to "<exeDir>\ScarsTool_dump\<timestamp>\".
// Returns the chosen output directory in outDirChosen.
bool DumpAllAuto(std::wstring& outDirChosen, Stats& stats);

} // namespace sdk_dumper

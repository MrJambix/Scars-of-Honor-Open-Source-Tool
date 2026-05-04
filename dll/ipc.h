// ════════════════════════════════════════════════════════════════════════════
// ipc.h  -  Tiny named-pipe command bridge for live diagnostics.
//
//   Pipe name : \\.\pipe\ScarsTool
//   Protocol  : line-framed.  One request per line "cmd arg1 arg2 ...\n".
//               Response is one line of JSON terminated by "\n".  Any newlines
//               inside the response are escaped as "\\n".
//
//   Handlers can opt-in to main-thread dispatch (runOnMainThread=true) for
//   anything that touches IL2CPP / game objects -- the pipe worker thread
//   blocks on a promise while ipc::Pump() (called from the renderer's tick
//   callback) drains the queue.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include <string>
#include <functional>

namespace ipc {

// Fill `out` with the JSON reply for the given args line.  Throwing or
// returning an empty `out` produces an empty response.
using Handler = std::function<void(const std::string& argsLine, std::string& out)>;

// Lifecycle.
void Install();      // start pipe server thread
void Shutdown();     // join + cleanup

// Main-thread pump.  Call once per frame from the renderer tick callback.
void Pump();

// Register a command handler.  `cmd` is matched as the first whitespace-
// delimited token of the request line.  When `runOnMainThread` is true the
// pipe thread enqueues the call and waits until Pump() invokes it.
void Register(const char* cmd, Handler h, bool runOnMainThread);

} // namespace ipc

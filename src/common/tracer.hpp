#pragma once
#include <cstdint>
#include <string>

// Research tool: "find what accesses this address" with a hardware data breakpoint (DR0) armed on every
// thread of the game for a few seconds. Results: %LOCALAPPDATA%\RE5TrainerPlus\trace_<label>.txt
// (instruction address after the access + hit count).
namespace tracer {
// mode: "w" = writes, "rw" = reads and writes. len: 1, 2 or 4.
bool Start(uintptr_t address, int len, const std::string& mode, int seconds, const std::string& label);
bool Busy();
void Shutdown();  // waits for a running trace, removes the exception handler (before unloading)
}

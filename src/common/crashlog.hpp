#pragma once

// Writes crash.log (data folder) when the game hits a fatal exception, before Windows closes it:
// address, registers, ebp frame chain, stack words, and the vtables of the objects in registers.
namespace crashlog {
void Install();
void Uninstall();
}

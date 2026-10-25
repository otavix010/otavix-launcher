// Closing a running game from outside it: its windows and its children's are asked to close (as the window's X
// does); what lingers can then be ended. A port's exe may start the real game as a child (GoW Betrayal.exe runs Java).
#pragma once
#include <cstdint>

namespace launcher {

// WM_CLOSE to the visible top-level windows of the process and its descendants; false when none was found
bool AskToClose(uint64_t pid);
// ends the process and its descendants
void EndProcessTree(uint64_t pid);

}  // namespace launcher

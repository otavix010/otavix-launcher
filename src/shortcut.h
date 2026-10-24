// A desktop shortcut that opens the launcher on one game.
#pragma once
#include <string>

#include "profile.h"

namespace launcher {

// name: the shortcut's file name (without .lnk); icon: an .exe or .ico, empty for the launcher's own; false and why
bool DesktopShortcut(const std::string& name, const std::string& args, const fs::path& icon, const fs::path& workdir,
                     std::string& error);

}  // namespace launcher

// A port's mods: each a folder in its mods folder whose files take the place of the game's own (same paths), the
// active ones and their order in mods/ativos.txt (the first wins), read by the port when it starts.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

struct Mod {
    std::string folder;  // its folder's name; for a .zip not yet installed, the file's name
    bool zip = false;
    Text name;           // mod.toml's, else the folder's name
    std::string author, version;
    Text description;
    bool active = false;
    std::vector<std::string> files;  // relative, with '/'
    uint64_t bytes = 0;
};

// the mods in a folder: the active ones first in their order, then the others by name, then .zip files
std::vector<Mod> ScanMods(const fs::path& dir);
// the active ones, in order, into ativos.txt
bool SaveActiveMods(const fs::path& dir, const std::vector<Mod>& mods);
// files that more than one active mod brings: the file, then the mods holding it in order
std::map<std::string, std::vector<std::string>> ModConflicts(const std::vector<Mod>& mods);
// a removed mod goes to _removidos, never deleted
bool RemoveMod(const fs::path& dir, const Mod& mod, std::string& error);
// after a .zip is unpacked into its folder: a single wrapping folder (without the game's own top folders) is lifted
void FlattenMod(const fs::path& folder);

}  // namespace launcher

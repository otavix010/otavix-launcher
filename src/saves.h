// Save backups: each a dated folder under backups\ beside the game, holding a copy of every save folder.
#pragma once
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

struct Backup {
    fs::path dir;
    std::string name;  // the folder's name: when it was made
    uint64_t bytes = 0;
};

// backups\ beside the game
fs::path BackupRoot(const fs::path& game_dir);
uint64_t FolderBytes(const fs::path& dir);
std::vector<Backup> ListBackups(const fs::path& game_dir);
// copies the profile's save folders; false when there was nothing to copy or the copy failed
bool MakeBackup(const fs::path& game_dir, const Profile& p, const std::string& tag, std::string& error);
// the current saves are backed up first, then replaced by the backup's
bool RestoreBackup(const fs::path& game_dir, const Profile& p, const Backup& b, std::string& error);
// the oldest automatic backups go past keep
void PruneAutomatic(const fs::path& game_dir, size_t keep);

}  // namespace launcher

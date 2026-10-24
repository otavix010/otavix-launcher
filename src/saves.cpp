#include "saves.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace launcher {

namespace {

fs::path BackupRoot(const fs::path& game_dir) { return game_dir / "backups"; }

std::string Stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d_%H-%M-%S", &tm);
    return b;
}

}  // namespace

uint64_t FolderBytes(const fs::path& dir) {
    uint64_t n = 0;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    return n;
}

std::vector<Backup> ListBackups(const fs::path& game_dir) {
    std::vector<Backup> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(BackupRoot(game_dir), ec))
        if (e.is_directory(ec)) out.push_back({e.path(), U8(e.path().filename()), FolderBytes(e.path())});
    std::sort(out.begin(), out.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
    return out;
}

bool MakeBackup(const fs::path& game_dir, const Profile& p, const std::string& tag, std::string& error) {
    std::error_code ec;
    bool any = false;
    for (const std::string& s : p.saves)
        if (FolderBytes(game_dir / FromU8(s))) any = true;
    if (!any) {
        error = "no saves";
        return false;
    }
    fs::path dst = BackupRoot(game_dir) / FromU8(Stamp() + (tag.empty() ? "" : "_" + tag));
    for (const std::string& s : p.saves) {
        fs::path from = game_dir / FromU8(s);
        if (!fs::is_directory(from, ec)) continue;
        fs::create_directories(dst / FromU8(s), ec);
        fs::copy(from, dst / FromU8(s), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = ec.message();
            return false;
        }
    }
    return true;
}

bool RestoreBackup(const fs::path& game_dir, const Profile& p, const Backup& b, std::string& error) {
    std::string ignored;
    MakeBackup(game_dir, p, "antes-de-restaurar", ignored);
    std::error_code ec;
    for (const std::string& s : p.saves) {
        fs::path from = b.dir / FromU8(s), to = game_dir / FromU8(s);
        if (!fs::is_directory(from, ec)) continue;
        // the folder is renamed aside rather than deleted until the copy has worked
        fs::path aside = to;
        aside += ".restaurando";
        fs::remove_all(aside, ec);
        if (fs::exists(to, ec)) fs::rename(to, aside, ec);
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = ec.message();
            fs::remove_all(to, ec);
            fs::rename(aside, to, ec);
            return false;
        }
        fs::remove_all(aside, ec);
    }
    return true;
}

void PruneAutomatic(const fs::path& game_dir, size_t keep) {
    std::vector<Backup> all = ListBackups(game_dir);
    size_t seen = 0;
    std::error_code ec;
    for (const Backup& b : all)
        if (b.name.size() > 5 && b.name.compare(b.name.size() - 5, 5, "_auto") == 0 && ++seen > keep)
            fs::remove_all(b.dir, ec);
}

}  // namespace launcher

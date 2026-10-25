// Installing a port's game from what the player downloaded: an archive (.7z, .zip, .rar: unpacked by the system's tar,
// libarchive), a disc image or a folder. The game's file named by the profile is checked against the release the port
// was made from (SHA-1), and the game ends up in the port's folder.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

std::string Sha1Hex(const uint8_t* data, size_t size);

// what can be picked: archives and disc images
bool IsArchive(const fs::path& p);
bool IsDiscImage(const fs::path& p);
// a game that is one file, by the profile's extensions
bool IsGameFile(const fs::path& p, const InstallSpec& spec);

class Installer {
public:
    enum class Stage { Idle, Reading, Extracting, Checking, Done, Failed };
    struct Status {
        Stage stage = Stage::Idle;
        float progress = 0;        // 0..1 while extracting
        std::string error;         // when failed
        fs::path game;             // when done: the disc image or folder to play
        bool version_ok = false;   // its file matched the profile's
        bool from_archive = false;
        fs::path archive;          // what was unpacked, to offer its removal
        uint64_t archive_bytes = 0;
    };
    // source: an archive, a disc image or a folder; dest: where an archive's game is kept
    void Start(const fs::path& source, const fs::path& dest, const InstallSpec& spec);
    Status Get();
    void Cancel();
    bool Busy();

private:
    void Run(fs::path source, fs::path dest, InstallSpec spec);
    std::mutex m_;
    Status st_;
    std::atomic<bool> cancel_ = false;
};

}  // namespace launcher

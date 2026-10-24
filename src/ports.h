// A port's own files from its GitHub release: the newest release's .zip, checked against the SHA-256 GitHub publishes
// for it, unpacked into the port's folder over the old files (saves, settings, mods and the game stay), its tag kept
// in port_version.txt to tell when a newer one is out.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "profile.h"

namespace launcher {

std::string InstalledPortVersion(const fs::path& dir);

class PortFetcher {
public:
    enum class Stage { Idle, Checking, Ready, Downloading, Unpacking, Done, Failed };
    struct Status {
        Stage stage = Stage::Idle;
        std::string latest;     // the newest release's tag
        uint64_t size = 0;      // its .zip
        uint64_t done = 0;      // downloaded so far
        std::string error;
    };
    void Check(const std::string& repo);                      // the newest release, on a thread
    void Install(const std::string& repo, const fs::path& dir);  // download and unpack it, on a thread
    void Cancel() { cancel_ = true; }
    Status Get();
    bool Busy();

private:
    bool Latest(const std::string& repo, std::string& url, std::string& digest);  // fills st_.latest and size
    std::mutex m_;
    Status st_;
    std::atomic<bool> cancel_ = false;
};

}  // namespace launcher

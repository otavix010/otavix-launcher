// Updates: the newest release of the launcher on GitHub against the installed version. Checking runs on a thread;
// updating downloads the release's installer (its SHA-256 checked against GitHub's), and the launcher runs it silently
// over itself and closes; the installer opens the new version when it is done.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace launcher {

struct UpdateCheck {
    enum class State { Idle, Checking, UpToDate, Newer, NoRepo, Failed, Downloading, Ready };
    State state = State::Idle;
    std::string latest;     // the release's tag
    std::string url;        // its page
    std::string error;
    std::string installer;  // the release's installer, its digest ("sha256:...") and size
    std::string digest;
    uint64_t size = 0;
    uint64_t done = 0;      // downloaded so far
    std::filesystem::path file;  // Ready: the installer downloaded
};

class Updater {
public:
    void Start(const std::string& repo, const std::string& installed);  // a check on a thread of its own
    void Download();  // after a Newer check: the installer, on a thread; Ready when it is in place
    UpdateCheck Get();

private:
    std::mutex m_;
    UpdateCheck cur_;
    std::atomic<bool> cancel_ = false;
};

// "v1.2.10" against "1.2.9": numbers compared in turn
bool NewerVersion(const std::string& latest, const std::string& installed);

// runs the downloaded installer over the launcher's own folder, silently, asking it to open the launcher afterwards
bool RunInstaller(const std::filesystem::path& installer, const std::filesystem::path& install_dir, std::string& error);

}  // namespace launcher

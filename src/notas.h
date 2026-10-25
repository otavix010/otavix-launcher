// Release notes: the launcher's and the ports' releases on GitHub, newest first, each with its text (Markdown), for
// the settings' "What's new" and the update pop-up. Each repository is looked up once a session, on a thread.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace launcher {

struct Release {
    std::string tag, name, date;  // date: "2026-10-25"
    std::string body;             // Markdown
};

class ReleaseNotes {
public:
    enum class State { None, Loading, Ready, Failed };
    void Fetch(const std::string& repo);  // once a session per repository
    State Get(const std::string& repo, std::vector<Release>* out = nullptr);

private:
    std::mutex m_;
    std::map<std::string, State> state_;
    std::map<std::string, std::vector<Release>> releases_;
};

// a release list's JSON (GitHub's), drafts left out
std::vector<Release> ParseReleases(const std::string& json);

}  // namespace launcher

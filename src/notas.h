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
    // a text file of a repository's branch as it is on GitHub (raw), once a session: a port's NOVIDADES.md, for what
    // its update brings before it is installed
    void FetchFile(const std::string& repo, const std::string& branch, const std::string& path);
    State GetFile(const std::string& repo, const std::string& branch, const std::string& path, std::string* out = nullptr);

private:
    std::mutex m_;
    std::map<std::string, State> state_;
    std::map<std::string, std::vector<Release>> releases_;
    std::map<std::string, State> file_state_;
    std::map<std::string, std::string> files_;
};

// a release list's JSON (GitHub's), drafts left out
std::vector<Release> ParseReleases(const std::string& json);

}  // namespace launcher

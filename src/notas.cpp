#include "notas.h"

#include <thread>

#include "desempenho.h"
#include "net.h"

namespace launcher {

namespace {

// the objects of the JSON array at the top, each as text
std::vector<std::string> Objects(const std::string& json) {
    std::vector<std::string> out;
    int depth = 0;
    bool in_string = false;
    size_t start = 0;
    for (size_t i = 0; i < json.size(); i++) {
        char c = json[i];
        if (in_string) {
            if (c == '\\') i++;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth++ == 0) start = i;
        } else if (c == '}') {
            if (--depth == 0) out.push_back(json.substr(start, i - start + 1));
        }
    }
    return out;
}

}  // namespace

std::vector<Release> ParseReleases(const std::string& json) {
    std::vector<Release> out;
    for (const std::string& o : Objects(json)) {
        double draft = 0;
        size_t at = o.find("\"draft\":");
        if (at != std::string::npos && o.compare(at + 8, 4, "true") == 0) draft = 1;
        if (draft) continue;
        Release r;
        r.tag = JsonString(o, "tag_name");
        r.name = JsonString(o, "name");
        r.date = JsonString(o, "published_at").substr(0, 10);
        r.body = JsonString(o, "body");
        if (!r.tag.empty()) out.push_back(r);
    }
    return out;
}

void ReleaseNotes::Fetch(const std::string& repo) {
    if (repo.empty()) return;
    {
        std::lock_guard lock(m_);
        if (state_[repo] != State::None) return;
        state_[repo] = State::Loading;
    }
    std::thread([this, repo] {
        HttpResult r = GitHubApi("repos/" + repo + "/releases?per_page=15");
        std::vector<Release> list = r.Ok() ? ParseReleases(r.body) : std::vector<Release>{};
        std::lock_guard lock(m_);
        releases_[repo] = std::move(list);
        state_[repo] = r.Ok() ? State::Ready : State::Failed;
    }).detach();
}

ReleaseNotes::State ReleaseNotes::Get(const std::string& repo, std::vector<Release>* out) {
    std::lock_guard lock(m_);
    auto s = state_.find(repo);
    if (s == state_.end()) return State::None;
    if (out && s->second == State::Ready) *out = releases_[repo];
    return s->second;
}

void ReleaseNotes::FetchFile(const std::string& repo, const std::string& branch, const std::string& path) {
    if (repo.empty() || branch.empty()) return;
    const std::string key = repo + "@" + branch + ":" + path;
    {
        std::lock_guard lock(m_);
        if (file_state_[key] != State::None) return;
        file_state_[key] = State::Loading;
    }
    std::thread([this, key, repo, branch, path] {
        // raw.githubusercontent.com is GitHub's file server, not its API: no 60-an-hour limit per address
        HttpResult r = HttpRequest("GET", "https://raw.githubusercontent.com/" + repo + "/" + branch + "/" + path, {}, "");
        std::lock_guard lock(m_);
        files_[key] = r.Ok() ? r.body : std::string();
        file_state_[key] = r.Ok() ? State::Ready : State::Failed;
    }).detach();
}

ReleaseNotes::State ReleaseNotes::GetFile(const std::string& repo, const std::string& branch, const std::string& path,
                                          std::string* out) {
    std::lock_guard lock(m_);
    auto s = file_state_.find(repo + "@" + branch + ":" + path);
    if (s == file_state_.end()) return State::None;
    if (out && s->second == State::Ready) *out = files_[s->first];
    return s->second;
}

}  // namespace launcher

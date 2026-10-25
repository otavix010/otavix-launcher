#include "update.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <thread>
#include <vector>

#include "net.h"

namespace launcher {

namespace fs = std::filesystem;

namespace {

std::vector<long> Numbers(const std::string& v) {
    std::vector<long> out;
    long n = -1;
    for (char c : v) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            n = (n < 0 ? 0 : n * 10) + (c - '0');
        } else if (n >= 0) {
            out.push_back(n);
            n = -1;
        }
    }
    if (n >= 0) out.push_back(n);
    return out;
}

// a field of GitHub's JSON, a string or a number, from a position on and before another
std::string Field(const std::string& json, const std::string& key, size_t from = 0, size_t to = std::string::npos) {
    size_t at = json.find("\"" + key + "\"", from);
    if (at == std::string::npos || at >= to) return {};
    at = json.find(':', at);
    size_t q = json.find_first_not_of(" \t\r\n", at + 1);
    if (q == std::string::npos) return {};
    if (json[q] != '"') return json.substr(q, json.find_first_of(",}\r\n", q) - q);
    std::string out;
    for (size_t i = q + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size()) i++;
        out += json[i];
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool NewerVersion(const std::string& latest, const std::string& installed) {
    std::vector<long> a = Numbers(latest), b = Numbers(installed);
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
        long x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

void Updater::Start(const std::string& repo, const std::string& installed) {
    {
        std::lock_guard lock(m_);
        if (cur_.state == UpdateCheck::State::Checking || cur_.state == UpdateCheck::State::Downloading) return;
        cur_ = {};
        if (repo.empty()) {
            cur_.state = UpdateCheck::State::NoRepo;
            return;
        }
        cur_.state = UpdateCheck::State::Checking;
    }
    std::thread([this, repo, installed] {
        UpdateCheck r;
        HttpResult h = GitHubApi("repos/" + repo + "/releases/latest");
        if (h.Ok()) {
            r.latest = Field(h.body, "tag_name");
            r.url = Field(h.body, "html_url");
            // the installer among the release's files: each asset ends with its browser_download_url
            size_t from = h.body.find("\"assets\"");
            for (size_t at = h.body.find("\"browser_download_url\"", from); at != std::string::npos;
                 from = at + 1, at = h.body.find("\"browser_download_url\"", from)) {
                std::string link = Field(h.body, "browser_download_url", at);
                if (Lower(link).size() < 4 || Lower(link).substr(link.size() - 4) != ".exe") continue;
                r.installer = link;
                r.size = std::strtoull(Field(h.body, "size", from, at).c_str(), nullptr, 10);
                r.digest = Field(h.body, "digest", from, at);
                break;
            }
            r.state = r.latest.empty()                  ? UpdateCheck::State::Failed
                      : NewerVersion(r.latest, installed) ? UpdateCheck::State::Newer
                                                          : UpdateCheck::State::UpToDate;
            if (r.latest.empty()) r.error = "resposta sem versão";
        } else {
            r.state = UpdateCheck::State::Failed;
            r.error = h.status == 404 ? "ainda não há versão publicada" : h.error;
        }
        std::lock_guard lock(m_);
        cur_ = r;
    }).detach();
}

void Updater::Download() {
    UpdateCheck c;
    {
        std::lock_guard lock(m_);
        if (cur_.state != UpdateCheck::State::Newer || cur_.installer.empty()) return;
        cur_.state = UpdateCheck::State::Downloading;
        cur_.done = 0;
        c = cur_;
    }
    cancel_ = false;
    std::thread([this, c] {
        std::error_code ec;
        fs::path file = fs::temp_directory_path(ec) / ("Instalar-Otavix-Launcher-" + c.latest + ".exe");
        std::string sha, error;
        bool ok = DownloadFile(c.installer, file, cancel_, cur_.done, m_, sha, error);
        // the installer must be the one GitHub says it published
        if (ok && c.digest.rfind("sha256:", 0) == 0 && Lower(c.digest.substr(7)) != sha) {
            ok = false;
            error = "o instalador baixado não confere (SHA-256); tente de novo";
        }
        if (!ok) fs::remove(file, ec);
        std::lock_guard lock(m_);
        cur_.state = ok ? UpdateCheck::State::Ready : UpdateCheck::State::Failed;
        cur_.error = error;
        cur_.file = file;
    }).detach();
}

UpdateCheck Updater::Get() {
    std::lock_guard lock(m_);
    return cur_;
}

bool RunInstaller(const fs::path& installer, const fs::path& install_dir, std::string& error) {
    // no separator at the end: before the quote Windows adds around it, it would escape that quote
    fs::path d = install_dir.has_filename() ? install_dir : install_dir.parent_path();
    std::string exe = installer.string(), dir = "/DIR=" + d.string();
    // silent, over this folder; the launcher closes now, the installer closes it if it lingers and opens the new one
    const char* args[] = {exe.c_str(), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS",
                          dir.c_str(), "/relaunch=1", nullptr};
    SDL_Process* p = SDL_CreateProcess(args, false);
    if (!p) {
        error = SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(p);  // it keeps running on its own
    return true;
}

}  // namespace launcher

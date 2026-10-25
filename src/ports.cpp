#include "ports.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include "net.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace launcher {

namespace {

// a string field of GitHub's JSON, from a position on
std::string Field(const std::string& json, const std::string& key, size_t from = 0, size_t to = std::string::npos) {
    size_t at = json.find("\"" + key + "\"", from);
    if (at == std::string::npos || at >= to) return {};
    at = json.find(':', at);
    size_t q = json.find_first_not_of(" \t\r\n", at + 1);
    if (q == std::string::npos) return {};
    if (json[q] != '"') {  // a number
        size_t e = json.find_first_of(",}\r\n", q);
        return json.substr(q, e - q);
    }
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

// the unpacked release over the port's folder: new and changed files in, the player's own untouched
void Merge(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::create_directories(to, ec);
    for (auto& e : fs::directory_iterator(from, ec)) {
        fs::path dst = to / e.path().filename();
        if (e.is_directory(ec)) Merge(e.path(), dst, ec);
        else fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return;
    }
}

// whether every file changed between two commits is documentation, which changes nothing of the port
bool OnlyDocs(const std::string& repo, const std::string& from, const std::string& to) {
    HttpResult r = HttpRequest("GET", "https://api.github.com/repos/" + repo + "/compare/" + from + "..." + to,
                               {{"Accept", "application/vnd.github+json"}}, "");
    size_t files = r.Ok() ? r.body.find("\"files\"") : std::string::npos;
    if (files == std::string::npos) return false;
    int n = 0;
    for (size_t at = r.body.find("\"filename\"", files); at != std::string::npos;
         at = r.body.find("\"filename\"", at + 1), n++) {
        std::string name = Lower(Field(r.body, "filename", at));
        bool doc = name.rfind("docs/", 0) == 0 || name.rfind(".github/", 0) == 0 || name == "license" ||
                   name == ".gitignore" || (name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0);
        if (!doc) return false;
    }
    return n > 0;
}

}  // namespace

std::string InstalledPortVersion(const fs::path& dir) {
    std::ifstream in(dir / "port_version.txt");
    std::string v;
    std::getline(in, v);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

PortFetcher::Status PortFetcher::Get() {
    std::lock_guard lock(m_);
    return st_;
}

bool PortFetcher::Busy() {
    Stage s = Get().stage;
    return s == Stage::Checking || s == Stage::Downloading || s == Stage::Unpacking;
}

bool PortFetcher::Latest(const std::string& source, std::string& url, std::string& digest,
                         const std::string& installed) {
    size_t at_branch = source.find('@');
    if (at_branch != std::string::npos) {
        std::string repo = source.substr(0, at_branch), branch = source.substr(at_branch + 1);
        HttpResult r = HttpRequest("GET", "https://api.github.com/repos/" + repo + "/commits/" + branch,
                                   {{"Accept", "application/vnd.github+json"}}, "");
        std::string sha = r.Ok() ? Field(r.body, "sha") : std::string();
        bool docs_only = sha.size() >= 12 && !installed.empty() && sha.compare(0, 12, installed) != 0 &&
                         OnlyDocs(repo, installed, sha);
        std::lock_guard lock(m_);
        if (sha.size() < 12) {
            st_.error = r.Ok() ? "resposta inesperada do GitHub" : r.error;
            return false;
        }
        st_.latest = docs_only ? installed : sha.substr(0, 12);
        st_.size = 0;
        url = "https://codeload.github.com/" + repo + "/zip/" + sha;
        digest.clear();
        return true;
    }
    const std::string& repo = source;
    HttpResult r = HttpRequest("GET", "https://api.github.com/repos/" + repo + "/releases/latest",
                               {{"Accept", "application/vnd.github+json"}}, "");
    std::lock_guard lock(m_);
    if (!r.Ok()) {
        st_.error = r.status == 404 ? "ainda não há versão publicada" : r.error;
        return false;
    }
    st_.latest = Field(r.body, "tag_name");
    // each asset ends with its browser_download_url: its name, size and digest come before it
    size_t from = r.body.find("\"assets\"");
    for (size_t at = r.body.find("\"browser_download_url\"", from); at != std::string::npos;
         from = at + 1, at = r.body.find("\"browser_download_url\"", from)) {
        std::string link = Field(r.body, "browser_download_url", at);
        if (Lower(link).size() < 4 || Lower(link).substr(link.size() - 4) != ".zip") continue;
        url = link;
        st_.size = std::strtoull(Field(r.body, "size", from, at).c_str(), nullptr, 10);
        digest = Field(r.body, "digest", from, at);  // "sha256:..."
        return true;
    }
    st_.error = "a versão publicada não tem o pacote .zip";
    return false;
}

void PortFetcher::Check(const std::string& repo, const std::string& installed) {
    {
        std::lock_guard lock(m_);
        if (st_.stage == Stage::Checking || st_.stage == Stage::Downloading || st_.stage == Stage::Unpacking) return;
        st_ = {};
        st_.stage = Stage::Checking;
    }
    std::thread([this, repo, installed] {
        std::string url, digest;
        bool ok = Latest(repo, url, digest, installed);
        std::lock_guard lock(m_);
        st_.stage = ok ? Stage::Ready : Stage::Failed;
    }).detach();
}

void PortFetcher::Install(const std::string& repo, const fs::path& dir) {
    {
        std::lock_guard lock(m_);
        if (st_.stage == Stage::Downloading || st_.stage == Stage::Unpacking) return;
        st_.stage = Stage::Downloading;
        st_.done = 0;
        st_.error.clear();
    }
    cancel_ = false;
    std::thread([this, repo, dir] {
        auto fail = [&](const std::string& why) {
            std::lock_guard lock(m_);
            st_.stage = Stage::Failed;
            st_.error = why;
        };
#ifdef _WIN32
        std::string url, digest;
        if (!Latest(repo, url, digest)) {
            std::lock_guard lock(m_);
            st_.stage = Stage::Failed;
            return;
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path zip = dir / "_port.zip", work = dir / "_port";
        std::string sha, error;
        if (!DownloadFile(url, zip, cancel_, st_.done, m_, sha, error)) {
            fs::remove(zip, ec);
            return fail(error);
        }
        // the package must be the one GitHub says it published
        if (digest.rfind("sha256:", 0) == 0 && Lower(digest.substr(7)) != sha) {
            fs::remove(zip, ec);
            return fail("o pacote baixado não confere (SHA-256); tente de novo");
        }
        {
            std::lock_guard lock(m_);
            st_.stage = Stage::Unpacking;
        }
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        char sys[512];
        const char* root = SDL_getenv("SystemRoot");
        std::snprintf(sys, sizeof(sys), "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
        std::string z = U8(zip), w = U8(work);
        const char* args[] = {sys, "-xf", z.c_str(), "-C", w.c_str(), nullptr};
        SDL_Process* p = SDL_CreateProcess(args, false);
        int code = -1;
        if (p) {
            SDL_WaitProcess(p, true, &code);
            SDL_DestroyProcess(p);
        }
        fs::remove(zip, ec);
        if (code != 0) {
            fs::remove_all(work, ec);
            return fail("falha ao descompactar o port (tar " + std::to_string(code) + ")");
        }
        // a package wrapped in one folder: that folder's contents
        fs::path top = work;
        std::vector<fs::path> entries;
        for (auto& e : fs::directory_iterator(work, ec)) entries.push_back(e.path());
        if (entries.size() == 1 && fs::is_directory(entries[0], ec)) top = entries[0];
        Merge(top, dir, ec);
        fs::remove_all(work, ec);
        if (ec) return fail("não foi possível copiar os arquivos: " + ec.message());
        std::string tag = Get().latest;
        std::ofstream(dir / "port_version.txt") << tag << "\n";
        std::lock_guard lock(m_);
        st_.stage = Stage::Done;
#else
        (void)repo, (void)dir;
        fail("not on this system yet");
#endif
    }).detach();
}

}  // namespace launcher

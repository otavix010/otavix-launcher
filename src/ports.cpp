#include "ports.h"
#include "i18n.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
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
    HttpResult r = GitHubApi("repos/" + repo + "/compare/" + from + "..." + to);
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
        HttpResult r = GitHubApi("repos/" + repo + "/commits/" + branch);
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
        // the server's copy of the commit (its "zip_url"), else GitHub's own zip
        url = Field(r.body, "zip_url");
        if (url.empty()) url = "https://codeload.github.com/" + repo + "/zip/" + sha;
        digest.clear();
        return true;
    }
    const std::string& repo = source;
    HttpResult r = GitHubApi("repos/" + repo + "/releases/latest");
    std::lock_guard lock(m_);
    if (!r.Ok()) {
        st_.error = r.status == 404 ? L3("ainda não há versão publicada", "no version published yet", "aún no hay versión publicada") : r.error;
        return false;
    }
    st_.latest = Field(r.body, "tag_name");
    // each asset ends with its browser_download_url: its name, size and digest come before it
    size_t from = r.body.find("\"assets\"");
    for (size_t at = r.body.find("\"browser_download_url\"", from); at != std::string::npos;
         from = at + 1, at = r.body.find("\"browser_download_url\"", from)) {
        std::string link = Field(r.body, "browser_download_url", at);
        // the package of this system: Windows takes a .zip; Linux / SteamOS the one with "linux" in its name (.tar.gz or .zip)
        const std::string low = Lower(link);
        auto ends = [&](const char* e) { size_t n = std::strlen(e); return low.size() >= n && low.compare(low.size() - n, n, e) == 0; };
#ifdef _WIN32
        if (!ends(".zip") || low.find("linux") != std::string::npos) continue;
#else
        if (low.find("linux") == std::string::npos || !(ends(".tar.gz") || ends(".zip"))) continue;
#endif
        url = link;
        st_.size = std::strtoull(Field(r.body, "size", from, at).c_str(), nullptr, 10);
        digest = Field(r.body, "digest", from, at);  // "sha256:..."
        return true;
    }
    st_.error = L3("a versão publicada não tem o pacote .zip", "the published version has no .zip package", "la versión publicada no tiene el paquete .zip");
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
        std::string url, digest;
        if (!Latest(repo, url, digest)) {
            std::lock_guard lock(m_);
            st_.stage = Stage::Failed;
            return;
        }
        std::error_code ec;
        fs::create_directories(dir, ec);
        const bool tgz = url.size() >= 7 && url.compare(url.size() - 7, 7, ".tar.gz") == 0;
        fs::path zip = dir / (tgz ? "_port.tar.gz" : "_port.zip"), work = dir / "_port";
        std::string sha, error;
        if (!DownloadFile(url, zip, cancel_, st_.done, m_, sha, error)) {
            fs::remove(zip, ec);
            return fail(error);
        }
        // the package must be the one GitHub says it published
        if (digest.rfind("sha256:", 0) == 0 && Lower(digest.substr(7)) != sha) {
            fs::remove(zip, ec);
            return fail(L3("o pacote baixado não confere (SHA-256); tente de novo", "the downloaded package does not match (SHA-256); try again", "el paquete descargado no coincide (SHA-256); inténtalo de nuevo"));
        }
        {
            std::lock_guard lock(m_);
            st_.stage = Stage::Unpacking;
        }
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
#ifdef _WIN32
        char sys[512];
        const char* root = SDL_getenv("SystemRoot");
        std::snprintf(sys, sizeof(sys), "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
#else
        const char* sys = "bsdtar";  // libarchive's, as on Windows: reads .zip and .tar.gz and keeps the permissions
#endif
        std::string z = U8(zip), w = U8(work);
        const char* args[] = {sys, "-xf", z.c_str(), "-C", w.c_str(), nullptr};
        SDL_Process* p = SDL_CreateProcess(args, false);
        int code = -1;
        std::string why;
        if (p) {
            SDL_WaitProcess(p, true, &code);
            SDL_DestroyProcess(p);
        } else {
            why = SDL_GetError();  // tar did not even start (reports 985 and 994: "tar -1")
        }
#ifdef _WIN32
        // tar.exe missing, blocked or failing on this PC: the .zip is unpacked by PowerShell instead
        if (code != 0 && !tgz) {
            fs::remove_all(work, ec);
            fs::create_directories(work, ec);
            auto quote = [](const std::string& t) {
                std::string o = "'";
                for (char c : t) o += c == '\'' ? std::string("''") : std::string(1, c);
                return o + "'";
            };
            std::string ps = std::string(root ? root : "C:\\Windows") + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
            std::string cmd = "$ProgressPreference='SilentlyContinue'; Expand-Archive -LiteralPath " + quote(z) + " -DestinationPath " + quote(w) + " -Force";
            const char* pargs[] = {ps.c_str(), "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", cmd.c_str(), nullptr};
            SDL_Process* q = SDL_CreateProcess(pargs, false);
            int pcode = -1;
            if (q) {
                SDL_WaitProcess(q, true, &pcode);
                SDL_DestroyProcess(q);
            }
            if (pcode == 0) code = 0;
        }
#endif
        fs::remove(zip, ec);
        if (code != 0) {
            fs::remove_all(work, ec);
            return fail(std::string("falha ao descompactar o port (") + (p ? "tar saiu com " + std::to_string(code) : "tar.exe não abriu: " + why) + ")");
        }
        // a package wrapped in one folder: that folder's contents
        fs::path top = work;
        std::vector<fs::path> entries;
        for (auto& e : fs::directory_iterator(work, ec)) entries.push_back(e.path());
        if (entries.size() == 1 && fs::is_directory(entries[0], ec)) top = entries[0];
        Merge(top, dir, ec);
        fs::remove_all(work, ec);
        if (ec) return fail(L3("não foi possível copiar os arquivos: ", "could not copy the files: ", "no se pudieron copiar los archivos: ") + ec.message());
        std::string tag = Get().latest;
        std::ofstream(dir / "port_version.txt") << tag << "\n";
        std::lock_guard lock(m_);
        st_.stage = Stage::Done;
    }).detach();
}

}  // namespace launcher

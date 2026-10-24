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
#include <bcrypt.h>
#include <winhttp.h>
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

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// a download into a file, its SHA-256 on the way; redirects (GitHub's assets go to their storage) followed
bool Download(const std::string& url, const fs::path& file, std::atomic<bool>& cancel, uint64_t& done, std::mutex& m,
              std::string& sha256, std::string& error) {
    std::wstring wurl = Wide(url);
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof(u);
    wchar_t host[256]{}, path[4096]{};
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) return error = "endereço inválido", false;
    HINTERNET s = WinHttpOpen(L"Otavix-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : nullptr;
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = q && WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(q, nullptr);
    DWORD status = 0, size = sizeof(status);
    if (ok) WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
    if (!ok || status != 200) {
        error = ok ? "o GitHub respondeu " + std::to_string(status) : "sem conexão (" + std::to_string(GetLastError()) + ")";
        ok = false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (ok) {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        std::vector<char> buf(1 << 20);
        for (DWORD avail = 0; ok && WinHttpQueryDataAvailable(q, &avail) && avail;) {
            if (cancel) {
                error = "cancelado";
                ok = false;
                break;
            }
            DWORD got = 0;
            if (!WinHttpReadData(q, buf.data(), std::min<DWORD>(avail, DWORD(buf.size())), &got) || !got) break;
            out.write(buf.data(), got);
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), got, 0);
            std::lock_guard lock(m);
            done += got;
        }
        if (!out) error = "disco cheio?", ok = false;
        uint8_t digest[32];
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        char hex[65];
        for (int i = 0; i < 32; i++) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        sha256 = hex;
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}
#endif

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

bool PortFetcher::Latest(const std::string& repo, std::string& url, std::string& digest) {
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

void PortFetcher::Check(const std::string& repo) {
    {
        std::lock_guard lock(m_);
        if (st_.stage == Stage::Checking || st_.stage == Stage::Downloading || st_.stage == Stage::Unpacking) return;
        st_ = {};
        st_.stage = Stage::Checking;
    }
    std::thread([this, repo] {
        std::string url, digest;
        bool ok = Latest(repo, url, digest);
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
        if (!Download(url, zip, cancel_, st_.done, m_, sha, error)) {
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

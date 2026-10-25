#include "net.h"

#include <cstdio>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <fstream>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
#endif

}  // namespace

HttpResult HttpRequest(const std::string& method, const std::string& url, const Headers& headers, const std::string& body) {
    HttpResult r;
#ifdef _WIN32
    std::wstring wurl = Wide(url);
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof(u);
    wchar_t host[256]{}, path[2048]{};
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    wchar_t extra[2048]{};
    u.lpszExtraInfo = extra;
    u.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &u)) {
        r.error = "endereço inválido";
        return r;
    }
    std::wstring full = std::wstring(path) + extra;
    HINTERNET s = WinHttpOpen(L"Otavix-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        r.error = "WinHTTP";
        return r;
    }
    WinHttpSetTimeouts(s, 5000, 8000, 15000, 30000);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, Wide(method).c_str(), full.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    std::wstring hs;
    for (auto& [k, v] : headers) hs += Wide(k) + L": " + Wide(v) + L"\r\n";
    if (!body.empty()) hs += L"Content-Type: application/json; charset=utf-8\r\n";
    bool ok = q &&
              WinHttpSendRequest(q, hs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hs.c_str(), DWORD(-1),
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()),
                                 DWORD(body.size()), 0) &&
              WinHttpReceiveResponse(q, nullptr);
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
        r.status = int(status);
        for (DWORD avail = 0; WinHttpQueryDataAvailable(q, &avail) && avail;) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(q, chunk.data(), avail, &got)) break;
            r.body.append(chunk, 0, got);
        }
        if (!r.Ok()) r.error = r.body.empty() ? "servidor respondeu " + std::to_string(status) : r.body;
    } else {
        r.error = "sem conexão com o servidor (" + std::to_string(GetLastError()) + ")";
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
#else
    (void)method, (void)url, (void)headers, (void)body;
    r.error = "not on this system yet";
#endif
    return r;
}

void Call::Start(const std::string& method, const std::string& url, const Headers& headers, const std::string& body) {
    uint64_t token;
    {
        std::lock_guard lock(m_);
        state_ = State::Busy;
        result_ = {};
        token = ++token_;
    }
    std::thread([=, this] {
        HttpResult r = HttpRequest(method, url, headers, body);
        std::lock_guard lock(m_);
        if (token != token_) return;
        result_ = std::move(r);
        state_ = State::Done;
    }).detach();
}

Call::State Call::Get(HttpResult* out) {
    std::lock_guard lock(m_);
    if (out) *out = result_;
    return state_;
}

void Call::Reset() {
    std::lock_guard lock(m_);
    state_ = State::Idle;
    result_ = {};
    token_++;
}

std::string Json(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);
                }
        }
    }
    return o + "\"";
}

std::vector<std::vector<std::string>> Lines(const std::string& body) {
    std::vector<std::vector<std::string>> out;
    size_t at = 0;
    while (at < body.size()) {
        size_t e = body.find('\n', at);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(at, e - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            std::vector<std::string> f;
            size_t p = 0;
            for (;;) {
                size_t t = line.find('\t', p);
                f.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
                if (t == std::string::npos) break;
                p = t + 1;
            }
            out.push_back(std::move(f));
        }
        at = e + 1;
    }
    return out;
}

#ifdef _WIN32
std::wstring WideOf(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string FileSha1(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return {};
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        if (in.gcount() > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(in.gcount()), 0);
    }
    uint8_t digest[20];
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[41];
    for (int i = 0; i < 20; i++) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// a download into a file, its SHA-256 on the way; redirects (GitHub's assets go to their storage) followed
bool DownloadFile(const std::string& url, const fs::path& file, std::atomic<bool>& cancel, uint64_t& done, std::mutex& m,
              std::string& sha256, std::string& error) {
    std::wstring wurl = WideOf(url);
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
    // https, or plain http for an old public server (the Xbox 360 store pictures)
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
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
#else
bool DownloadFile(const std::string&, const fs::path&, std::atomic<bool>&, uint64_t&, std::mutex&, std::string&,
                  std::string& error) {
    error = "not on this system yet";
    return false;
}
std::string FileSha1(const fs::path&) { return {}; }
#endif

}  // namespace launcher

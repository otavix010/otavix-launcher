#include "update.h"

#include <algorithm>
#include <cctype>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace launcher {

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

// the string value of a key in GitHub's JSON, which this needs no parser for
std::string JsonString(const std::string& json, const std::string& key) {
    size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return {};
    at = json.find(':', at);
    if (at == std::string::npos) return {};
    at = json.find('"', at);
    if (at == std::string::npos) return {};
    std::string out;
    for (size_t i = at + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size()) i++;
        out += json[i];
    }
    return out;
}

#ifdef _WIN32
bool HttpsGet(const std::wstring& host, const std::wstring& path, std::string& body, std::string& error) {
    HINTERNET s = WinHttpOpen(L"Otavix-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return error = "WinHTTP", false;
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    bool ok = r && WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json", DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD) &&
              WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(r, nullptr);
    DWORD status = 0, size = sizeof(status);
    if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
    if (ok) {
        for (DWORD avail = 0; WinHttpQueryDataAvailable(r, &avail) && avail;) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got)) break;
            body.append(chunk, 0, got);
        }
    }
    if (!ok) error = "sem conexão com o GitHub (" + std::to_string(GetLastError()) + ")";
    else if (status != 200) error = "GitHub respondeu " + std::to_string(status), ok = false;
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
#endif

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
        if (cur_.state == UpdateCheck::State::Checking) return;
        cur_ = {};
        if (repo.empty()) {
            cur_.state = UpdateCheck::State::NoRepo;
            return;
        }
        cur_.state = UpdateCheck::State::Checking;
    }
    std::thread([this, repo, installed] {
        UpdateCheck r;
#ifdef _WIN32
        std::string body, error;
        std::wstring path = L"/repos/" + std::wstring(repo.begin(), repo.end()) + L"/releases/latest";
        if (HttpsGet(L"api.github.com", path, body, error)) {
            r.latest = JsonString(body, "tag_name");
            r.url = JsonString(body, "html_url");
            r.state = r.latest.empty() ? UpdateCheck::State::Failed
                      : NewerVersion(r.latest, installed) ? UpdateCheck::State::Newer
                                                         : UpdateCheck::State::UpToDate;
            if (r.latest.empty()) r.error = "resposta sem versão";
        } else {
            r.state = UpdateCheck::State::Failed;
            r.error = error;
        }
#else
        r.state = UpdateCheck::State::Failed;
        r.error = "not on this system yet";
#endif
        std::lock_guard lock(m_);
        cur_ = r;
    }).detach();
}

UpdateCheck Updater::Get() {
    std::lock_guard lock(m_);
    return cur_;
}

}  // namespace launcher

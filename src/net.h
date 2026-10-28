// The launcher's server: reports and port requests. Requests go as JSON; answers come back as lines of tab-separated
// fields, which need no parser.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace launcher {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResult {
    int status = 0;  // 0: no answer
    std::string body;
    std::string error;
    bool Ok() const { return status >= 200 && status < 300; }
};

// http:// or https://, blocking
HttpResult HttpRequest(const std::string& method, const std::string& url, const Headers& headers, const std::string& body);

// GitHub's API (a path such as "repos/<owner>/<repo>/releases/latest"), blocking: through the launcher's server when
// there is one (it keeps the answers and has ways round GitHub's 60-an-hour limit per address, which players sharing
// an address hit), else, or when the server cannot answer, straight from GitHub
void SetGitHubProxy(const std::string& server);
HttpResult GitHubApi(const std::string& path);
// a file the launcher's server keeps (/v1/arquivos/<key>; 28/10: the GitHub account was suspended), "" with no server
std::string ServerFile(const std::string& key);
// GET the server's copy first, then the GitHub address
HttpResult GetMirrored(const std::string& key, const std::string& github_url);

// one request at a time, in the background
class Call {
public:
    enum class State { Idle, Busy, Done };
    void Start(const std::string& method, const std::string& url, const Headers& headers, const std::string& body);
    State Get(HttpResult* out = nullptr);
    void Reset();

private:
    std::mutex m_;
    State state_ = State::Idle;
    HttpResult result_;
    uint64_t token_ = 0;
};

// a JSON string, quoted
std::string Json(const std::string& s);
// a download into a file, its SHA-256 (hex) worked out on the way; done grows under m as bytes land; redirects followed
bool DownloadFile(const std::string& url, const std::filesystem::path& file, std::atomic<bool>& cancel, uint64_t& done,
                  std::mutex& m, std::string& sha256, std::string& error);

// a public picture's addresses, the fastest first: libretro's thumbnails also come from their repositories on GitHub,
// whose CDN answers several times sooner (0.3 s against 2 s a box art, measured 27/10/2026); the address given last
std::vector<std::string> PictureMirrors(const std::string& url);
// a public picture downloaded into a file, trying its mirrors in turn; status: the last answer's (404: none has it)
bool DownloadPicture(const std::string& url, const std::filesystem::path& file, int* status = nullptr);

// a file's SHA-1 (hex); empty when it cannot be read
std::string FileSha1(const std::filesystem::path& file);

// an answer's lines, each split at its tabs
std::vector<std::vector<std::string>> Lines(const std::string& body);

}  // namespace launcher

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

// a file's SHA-1 (hex); empty when it cannot be read
std::string FileSha1(const std::filesystem::path& file);

// an answer's lines, each split at its tabs
std::vector<std::vector<std::string>> Lines(const std::string& body);

}  // namespace launcher

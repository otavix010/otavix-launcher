#include "shaders.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <atomic>
#include <thread>

#include <toml++/toml.hpp>

namespace launcher {

namespace {

const size_t kSendMax = 1'900'000;  // the server keeps a file under D1's 2 MB a value

// '*' any run of characters, '?' one; case does not matter
bool Match(const char* pat, const char* s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (const char* t = s;; t++) {
                if (Match(pat + 1, t)) return true;
                if (!*t) return false;
            }
        }
        if (!*s) return false;
        if (*pat != '?' && std::tolower(static_cast<unsigned char>(*pat)) != std::tolower(static_cast<unsigned char>(*s)))
            return false;
    }
    return !*s;
}

bool Wanted(const ShaderSpec& spec, const std::string& name) {
    for (const std::string& p : spec.files)
        if (Match(p.c_str(), name.c_str())) return true;
    return spec.files.empty();
}

bool ReadAll(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool WriteAll(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::path part = p;
    part += ".part";
    {
        std::ofstream f(part, std::ios::binary);
        if (!f.write(data.data(), std::streamsize(data.size()))) return false;
    }
    fs::rename(part, p, ec);
    if (ec) fs::remove(part, ec);
    return !ec;
}

uint64_t Le64(const std::string& b, size_t at) {
    uint64_t v = 0;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}

uint32_t Le32(const std::string& b, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

// a ReXGlue storage file's records: where each starts and its hash; false when the file does not end on a whole
// record (then it is left alone: what came after a cut record would be read wrong, and the game truncates it)
struct Record {
    size_t at, size;
    uint64_t hash;
};

bool XenosRecords(const std::string& b, bool shaders, size_t record_size, std::vector<Record>& out) {
    size_t head = shaders ? 8 : 12;
    if (b.size() < head) return false;
    size_t at = head;
    while (at < b.size()) {
        size_t size = 0;
        if (shaders) {
            // { u64 hash of the ucode, u32 dword count : 31 | type : 1 } then the ucode
            if (at + 12 > b.size()) return false;
            size = 12 + size_t(Le32(b, at + 8) & 0x7FFFFFFFu) * 4;
        } else {
            // { u64 hash of the description, the description }, all of one size
            if (!record_size) return false;
            size = record_size;
        }
        if (at + size > b.size()) return false;
        out.push_back({at, size, Le64(b, at)});
        at += size;
    }
    return true;
}

// the pack's storage file merged into the cache's own: the cache's records stay as they are and the pack's it lacks
// go after them. Only into a file with the very same header (another one is ReXGlue's other version, which would
// truncate it to nothing), and only whole records. Returns the records the pack brought, -1 when it was left alone
int MergeXenos(const fs::path& mine, const std::string& pack, bool shaders, size_t record_size) {
    std::error_code ec;
    std::vector<Record> theirs;
    if (!XenosRecords(pack, shaders, record_size, theirs)) return -1;
    std::string own;
    if (!fs::exists(mine, ec)) return WriteAll(mine, pack) ? int(theirs.size()) : -1;
    if (!ReadAll(mine, own)) return -1;
    size_t head = shaders ? 8 : 12;
    if (own.size() < head || own.compare(0, head, pack, 0, head) != 0) return -1;
    std::vector<Record> have;
    if (!XenosRecords(own, shaders, record_size, have)) return -1;
    std::set<uint64_t> known;
    for (const Record& r : have) known.insert(r.hash);
    std::string out = own;
    int added = 0;
    for (const Record& r : theirs)
        if (known.insert(r.hash).second) {
            out.append(pack, r.at, r.size);
            added++;
        }
    if (added && !WriteAll(mine, out)) return -1;
    return int(theirs.size());
}

std::string Tar() {
    const char* root = SDL_getenv("SystemRoot");
    return std::string(root ? root : "C:\\Windows") + "\\System32\\tar.exe";
}

std::string Today() {
    SDL_Time now = 0;
    SDL_DateTime d{};
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &d, true)) return {};
    char b[16];
    std::snprintf(b, sizeof(b), "%02d/%02d/%04d", d.day, d.month, d.year);
    return b;
}

// shaders_enviados.txt (sent) and shaders_do_pacote.txt (came from the pack): a line per file, its name, and for a
// storage file the size it had ("gowa" files never change; a storage file grows)
std::set<std::string> ReadLines(const fs::path& p) {
    std::set<std::string> out;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) out.insert(line);
    return out;
}

void AppendLine(const fs::path& p, const std::string& line) {
    std::ofstream f(p, std::ios::app);
    f << line << "\n";
}

}  // namespace

ShaderSpec ReadShaderSpec(const fs::path& profile_file) {
    ShaderSpec s;
    toml::table t;
    try {
        t = toml::parse_file(U8(profile_file));
    } catch (...) {
        return s;
    }
    const toml::table* a = t["shaders"].as_table();
    if (!a) return s;
    s.kind = (*a)["kind"].value_or(std::string());
    s.folder = (*a)["folder"].value_or(std::string());
    if (const toml::array* arr = (*a)["files"].as_array())
        for (auto& e : *arr)
            if (auto v = e.value<std::string>()) s.files.push_back(*v);
    if (s.kind != "gowa" && s.kind != "xenia") s.kind.clear();
    return s;
}

void ShaderShare::Load(const fs::path& data_dir) {
    std::ifstream f(data_dir / "shaders_pacote.txt");
    std::string sha;
    int count = -1;
    std::string day;
    f >> sha >> count >> day;
    int sent = int(ReadLines(data_dir / "shaders_enviados.txt").size());
    std::lock_guard lock(m_);
    if (f || !sha.empty()) st_.pack_count = count, st_.pack_day = day;
    st_.sent = sent;
}

void ShaderShare::Apply(const std::string& repo, const ShaderSpec& spec, const fs::path& port_dir, const fs::path& data_dir) {
    if (repo.empty() || spec.Empty()) return;
    {
        std::lock_guard lock(m_);
        if (st_.busy) return;
        st_.busy = true;
    }
    std::thread([this, repo, spec, port_dir, data_dir] {
        auto done = [this] {
            std::lock_guard lock(m_);
            st_.busy = false;
        };
        std::error_code ec;
        // tests: LAUNCHER_SHADERS_BASE=<address ending in />, the pack from there instead of the repository's release
        const char* test_base = SDL_getenv("LAUNCHER_SHADERS_BASE");
        std::string base = test_base ? std::string(test_base) : "https://github.com/" + repo + "/releases/download/shaders/";
        HttpResult r;
        // the server's copy first (28/10: the GitHub account was suspended), both files from the same place
        const std::string mirror = test_base ? std::string() : ServerFile("shaders/" + repo + "/");
        if (!mirror.empty()) {
            r = HttpRequest("GET", mirror + "shaders.zip.sha256", {}, "");
            if (r.Ok()) base = mirror;
        }
        if (base != mirror) r = HttpRequest("GET", base + "shaders.zip.sha256", {}, "");
        std::string want = r.Ok() ? r.body.substr(0, std::min<size_t>(r.body.size(), 64)) : std::string();
        for (char& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (want.size() != 64 || want.find_first_not_of("0123456789abcdef") != std::string::npos) return done();
        std::string had;
        {
            std::ifstream f(data_dir / "shaders_pacote.txt");
            f >> had;
        }
        if (had == want) return done();
        fs::create_directories(data_dir, ec);
        fs::path zip = data_dir / "shaders.zip", tmp = data_dir / "shaders_pacote";
        std::atomic<bool> cancel = false;
        uint64_t got = 0;
        std::mutex m;
        std::string sha, error;
        if (!DownloadFile(base + "shaders.zip", zip, cancel, got, m, sha, error) || sha != want) {
            fs::remove(zip, ec);
            return done();
        }
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        std::string tar = Tar(), src = U8(zip), to = U8(tmp);
        const char* args[] = {tar.c_str(), "-xf", src.c_str(), "-C", to.c_str(), nullptr};
        SDL_Process* p = SDL_CreateProcess(args, false);
        int code = -1;
        if (p) SDL_WaitProcess(p, true, &code), SDL_DestroyProcess(p);
        fs::remove(zip, ec);
        if (code != 0) {
            fs::remove_all(tmp, ec);
            return done();
        }
        fs::path cache = port_dir / FromU8(spec.folder);
        fs::create_directories(cache, ec);
        int count = 0;
        std::string from_pack;  // what the cache got from the pack, never sent back
        if (spec.kind == "gowa") {
            // one file per shader: those the cache lacks are copied in, the cache's own never touched; the pack's
            // names count as sent (nothing to send back)
            for (auto& e : fs::recursive_directory_iterator(tmp, ec)) {
                std::string name = U8(e.path().filename());
                if (!e.is_regular_file(ec) || !Wanted(spec, name)) continue;
                count++;
                fs::path dest = cache / e.path().filename();
                if (!fs::exists(dest, ec)) fs::copy_file(e.path(), dest, ec);
                from_pack += name + '\n';
            }
        } else {
            // the storage files, each merged; the pipelines' record size comes in the pack's index
            toml::table index;
            try {
                index = toml::parse_file(U8(tmp / "indice.toml"));
            } catch (...) {
            }
            for (auto& e : fs::directory_iterator(tmp, ec)) {
                std::string name = U8(e.path().filename());
                if (!e.is_regular_file(ec) || !Wanted(spec, name)) continue;
                bool shaders = Match("*.xsh", name.c_str());
                size_t record = size_t(index["arquivos"][name]["registro"].value_or(int64_t(0)));
                std::string pack;
                if (!ReadAll(e.path(), pack)) continue;
                int n = MergeXenos(cache / e.path().filename(), pack, shaders, record);
                if (n < 0) continue;
                count += n;
                // as it is now is not sent: what came from the pack does not go back
                from_pack += name + " " + std::to_string(fs::file_size(cache / e.path().filename(), ec)) + '\n';
            }
        }
        fs::remove_all(tmp, ec);
        WriteAll(data_dir / "shaders_do_pacote.txt", from_pack);
        std::string day = Today();
        WriteAll(data_dir / "shaders_pacote.txt", want + " " + std::to_string(count) + " " + day + "\n");
        int sent = int(ReadLines(data_dir / "shaders_enviados.txt").size());
        std::lock_guard lock(m_);
        st_.pack_count = count;
        st_.pack_day = day;
        st_.sent = sent;
        st_.busy = false;
    }).detach();
}

void ShaderShare::Send(const std::string& server, const Headers& headers, const std::string& game, const ShaderSpec& spec,
                       const fs::path& port_dir, const fs::path& data_dir) {
    if (server.empty() || spec.Empty()) return;
    {
        std::lock_guard lock(m_);
        if (st_.busy) return;
        st_.busy = true;
    }
    stop_ = false;
    std::thread([this, server, headers, game, spec, port_dir, data_dir] {
        std::error_code ec;
        fs::path cache = port_dir / FromU8(spec.folder);
        fs::create_directories(data_dir, ec);
        std::set<std::string> sent = ReadLines(data_dir / "shaders_enviados.txt");
        const std::set<std::string> pack = ReadLines(data_dir / "shaders_do_pacote.txt");
        Headers h = headers;
        h.push_back({"Content-Type", "application/octet-stream"});
        int n = 0, last = -1;
        size_t candidates = 0;
        for (auto& e : fs::directory_iterator(cache, ec)) {
            std::string name = U8(e.path().filename());
            if (!e.is_regular_file(ec) || !Wanted(spec, name)) continue;
            uint64_t size = e.file_size(ec);
            // a shader file once; a storage file each time it grew
            std::string mark = spec.kind == "gowa" ? name : name + " " + std::to_string(size);
            if (stop_) break;
            if (sent.count(mark) || pack.count(mark) || !size || size > kSendMax) continue;
            std::string body;
            if (!ReadAll(e.path(), body)) continue;
            candidates++;
            HttpResult r = HttpRequest("POST", server + "/v1/shaders?jogo=" + game + "&arquivo=" + name, h, body);
            last = r.status;
            if (r.status == 429 || r.status == 0) break;  // enough for this hour, or no server: next time
            if (!r.Ok()) continue;
            AppendLine(data_dir / "shaders_enviados.txt", mark);
            sent.insert(mark);
            if (++n >= 3000) break;
        }
        // the last round, for a report: what there was to send, what went, the server's last answer
        AppendLine(data_dir / "shaders_envio.log", Today() + " novos " + std::to_string(candidates) + ", enviados " +
                                                       std::to_string(n) + ", resposta " + std::to_string(last) +
                                                       (ec ? ", pasta: " + ec.message() : std::string()));
        int total = int(ReadLines(data_dir / "shaders_enviados.txt").size());
        std::lock_guard lock(m_);
        st_.sent = total;
        st_.busy = false;
    }).detach();
}

void ShaderShare::Wait() {
    for (int i = 0; i < 600; i++) {  // a minute at most
        {
            std::lock_guard lock(m_);
            if (!st_.busy) return;
        }
        SDL_Delay(100);
    }
}

ShaderShare::Status ShaderShare::Get() {
    std::lock_guard lock(m_);
    return st_;
}

}  // namespace launcher

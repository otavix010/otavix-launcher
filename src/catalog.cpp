#include "catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <thread>

#include "net.h"
#include "stb_image.h"

namespace launcher {

namespace {

// Latin-1 letters with accents to their plain letter
char Plain(uint32_t c) {
    if ((c >= 0xC0 && c <= 0xC5) || (c >= 0xE0 && c <= 0xE5)) return 'a';
    if (c == 0xC7 || c == 0xE7) return 'c';
    if ((c >= 0xC8 && c <= 0xCB) || (c >= 0xE8 && c <= 0xEB)) return 'e';
    if ((c >= 0xCC && c <= 0xCF) || (c >= 0xEC && c <= 0xEF)) return 'i';
    if (c == 0xD1 || c == 0xF1) return 'n';
    if ((c >= 0xD2 && c <= 0xD6) || c == 0xD8 || (c >= 0xF2 && c <= 0xF6) || c == 0xF8) return 'o';
    if ((c >= 0xD9 && c <= 0xDC) || (c >= 0xF9 && c <= 0xFC)) return 'u';
    if (c == 0xDD || c == 0xFD || c == 0xFF) return 'y';
    return 0;
}

const char* kRoman[][2] = {{"ii", "2"}, {"iii", "3"}, {"iv", "4"}, {"vi", "6"}, {"vii", "7"}, {"viii", "8"}, {"ix", "9"}};

const std::map<std::string, std::string>& LibretroSystems() {
    static const std::map<std::string, std::string> kSystems = {
        {"xbox", "Microsoft - Xbox"},      {"ps1", "Sony - PlayStation"},       {"ps2", "Sony - PlayStation 2"},
        {"psp", "Sony - PlayStation Portable"}, {"vita", "Sony - PlayStation Vita"}, {"gc", "Nintendo - GameCube"},
        {"wii", "Nintendo - Wii"},         {"wiiu", "Nintendo - Wii U"},       {"nds", "Nintendo - Nintendo DS"},
        {"3ds", "Nintendo - Nintendo 3DS"}};
    return kSystems;
}

std::string UrlPart(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += char(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            o += b;
        }
    }
    return o;
}

std::string CoverUrl(const std::string& console, const std::string& cover) {
    if (cover.rfind("gametdb:", 0) == 0) return "https://art.gametdb.com/ps3/coverM/" + cover.substr(8) + ".jpg";
    auto it = LibretroSystems().find(console);
    if (it == LibretroSystems().end()) return {};
    return "https://thumbnails.libretro.com/" + UrlPart(it->second) + "/Named_Boxarts/" + UrlPart(cover) + ".png";
}

std::string FileId(const std::string& id) {
    // FNV-1a: a name the file system takes
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : id) h = (h ^ c) * 1099511628211ull;
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(h));
    return b;
}

}  // namespace

std::string SearchKey(const std::string& s) {
    std::string k;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
    while (*p) {
        uint32_t c = *p;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (len == 2 && p[1]) c = (c & 0x1F) << 6 | (p[1] & 0x3F);
        char out = 0;
        if (c < 0x80) out = char(std::tolower(int(c)));
        else if (char pl = Plain(c)) out = pl;
        if (out && std::isalnum(static_cast<unsigned char>(out))) k += out;
        else if (!k.empty() && k.back() != ' ') k += ' ';
        for (int i = 0; i < len && *p; i++) p++;
    }
    while (!k.empty() && k.back() == ' ') k.pop_back();
    // roman numerals as digits, word by word
    std::string o, w;
    auto flush = [&] {
        for (auto& r : kRoman)
            if (w == r[0]) w = r[1];
        if (!w.empty()) o += (o.empty() ? "" : " ") + w;
        w.clear();
    };
    for (char c : k) {
        if (c == ' ') flush();
        else w += c;
    }
    flush();
    return o;
}

const std::vector<CatalogGame>& Catalogs::Of(const std::string& console) {
    auto it = lists_.find(console);
    if (it != lists_.end()) return it->second;
    std::vector<CatalogGame>& list = lists_[console];
    std::ifstream in(folder_ / FromU8(console + ".tsv"), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t t = line.find('\t');
        CatalogGame g;
        g.name = line.substr(0, t);
        g.cover = t == std::string::npos ? std::string() : line.substr(t + 1);
        g.key = SearchKey(g.name);
        list.push_back(std::move(g));
    }
    return list;
}

std::vector<const CatalogGame*> Catalogs::Search(const std::string& console, const std::string& typed, size_t n) {
    std::string q = SearchKey(typed);
    std::vector<std::pair<int, const CatalogGame*>> hits;
    if (q.empty()) return {};
    std::vector<std::string> words;
    for (size_t a = 0; a < q.size();) {
        size_t b = q.find(' ', a);
        words.push_back(q.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    for (const CatalogGame& g : Of(console)) {
        bool all = true;
        for (const std::string& w : words)
            if (g.key.find(w) == std::string::npos) {
                all = false;
                break;
            }
        if (!all) continue;
        // the name itself first, then names starting with it, then the shorter
        int score = g.key == q ? 0 : g.key.rfind(q, 0) == 0 ? 1 : g.key.find(q) != std::string::npos ? 2 : 3;
        hits.push_back({score * 1000 + int(std::min<size_t>(g.key.size(), 999)), &g});
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<const CatalogGame*> out;
    for (size_t i = 0; i < hits.size() && i < n; i++) out.push_back(hits[i].second);
    return out;
}

void Covers::Open(SDL_Renderer* ren, const fs::path& cache) {
    ren_ = ren;
    cache_ = cache;
    std::error_code ec;
    fs::create_directories(cache_, ec);
}

SDL_Texture* Covers::Get(const std::string& console, const std::string& cover, float* aspect, bool gray) {
    if (cover.empty()) return nullptr;
    std::string id = console + "/" + cover;
    if (auto it = tex_.find(id); it != tex_.end()) {
        if (aspect) *aspect = it->second.aspect;
        return gray ? it->second.gray : it->second.color;
    }
    std::lock_guard lock(m_);
    if (!asked_.insert(id).second) return nullptr;
    std::string url = CoverUrl(console, cover);
    if (url.empty()) return nullptr;
    queue_.push_back({id, url});
    // four at a time
    if (workers_ < 4) {
        workers_++;
        std::thread([this] { Worker(); }).detach();
    }
    return nullptr;
}

void Covers::Worker() {
    for (;;) {
        std::pair<std::string, std::string> job;
        {
            std::lock_guard lock(m_);
            if (queue_.empty() || closing_) {
                workers_--;
                return;
            }
            // the newest first: what the player just typed
            job = queue_.back();
            queue_.pop_back();
        }
        fs::path file = cache_ / FileId(job.first);
        std::vector<uint8_t> bytes;
        std::error_code ec;
        if (fs::is_regular_file(file, ec)) {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        } else {
            HttpResult r = HttpRequest("GET", job.second, {}, "");
            // GameTDB keeps a cover under its language: the others when the expected one has none
            size_t at = job.second.find("/coverM/");
            for (const char* reg : {"US", "EN", "JA", "DE", "FR", "ES", "IT", "KO"}) {
                if (r.Ok() || at == std::string::npos) break;
                std::string alt = job.second.substr(0, at + 8) + reg + job.second.substr(job.second.find('/', at + 8));
                if (alt != job.second) r = HttpRequest("GET", alt, {}, "");
            }
            if (!r.Ok() || r.body.empty()) continue;
            bytes.assign(r.body.begin(), r.body.end());
            std::ofstream(file, std::ios::binary).write(r.body.data(), std::streamsize(r.body.size()));
        }
        int w, h, n;
        uint8_t* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
        if (!px) continue;
        Decoded d;
        d.id = job.first;
        d.w = w;
        d.h = h;
        d.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
        stbi_image_free(px);
        // the grey one: luminance, alpha kept; every other pixel past 320 wide
        int step = w > 320 ? 2 : 1;
        d.gw = w / step;
        d.gh = h / step;
        d.gray.resize(size_t(d.gw) * size_t(d.gh) * 4);
        for (int y = 0; y < d.gh; y++)
            for (int x = 0; x < d.gw; x++) {
                const uint8_t* s = &d.rgba[(size_t(y * step) * size_t(w) + size_t(x * step)) * 4];
                uint8_t l = uint8_t((s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8);
                uint8_t* o = &d.gray[(size_t(y) * size_t(d.gw) + size_t(x)) * 4];
                o[0] = o[1] = o[2] = l;
                o[3] = s[3];
            }
        std::lock_guard lock(m_);
        done_.push_back(std::move(d));
    }
}

void Covers::Take() {
    std::vector<Decoded> done;
    {
        std::lock_guard lock(m_);
        done.swap(done_);
    }
    auto make = [this](int w, int h, const std::vector<uint8_t>& rgba) -> SDL_Texture* {
        if (!w || !h) return nullptr;
        SDL_Texture* t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t) return nullptr;
        SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        return t;
    };
    for (Decoded& d : done) {
        SDL_Texture* t = make(d.w, d.h, d.rgba);
        if (!t) continue;
        tex_[d.id] = {t, make(d.gw, d.gh, d.gray), float(d.w) / float(d.h)};
    }
}

void Covers::Close() {
    {
        std::lock_guard lock(m_);
        closing_ = true;
    }
    for (auto& [id, t] : tex_) {
        SDL_DestroyTexture(t.color);
        if (t.gray) SDL_DestroyTexture(t.gray);
    }
    tex_.clear();
}

}  // namespace launcher

#include "art.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <atomic>
#include <fstream>
#include <mutex>

#include "net.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

namespace launcher {

namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

bool ReadAll(const fs::path& p, std::vector<uint8_t>& out, size_t limit = SIZE_MAX) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    if (limit == SIZE_MAX) {
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }
    out.resize(limit);
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(limit));
    out.resize(size_t(f.gcount()));
    return !out.empty();
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// ------------------------------------------------------------------ Xbox disc image (XDVDFS)

constexpr uint64_t kSector = 2048;

struct Xiso {
    std::ifstream f;
    uint64_t base = 0;  // the game partition's offset in the image

    bool Open(const fs::path& p) {
        f.open(p, std::ios::binary);
        if (!f) return false;
        // a game partition alone (xiso), or a whole disc's with the partition at one of these offsets
        for (uint64_t b : {0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull}) {
            char magic[20];
            f.seekg(std::streamoff(b + 32 * kSector));
            if (f.read(magic, 20) && !std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20)) {
                base = b;
                return true;
            }
            f.clear();
        }
        return false;
    }

    bool Read(uint64_t at, size_t n, std::vector<uint8_t>& out) {
        out.resize(n);
        f.clear();
        f.seekg(std::streamoff(base + at));
        return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n)));
    }

    // name in the directory table at sector, size bytes: its sector and size
    bool Find(uint32_t sector, uint32_t size, const std::string& name, uint32_t& out_sector, uint32_t& out_size, bool& dir) {
        std::vector<uint8_t> t;
        if (!Read(uint64_t(sector) * kSector, size, t)) return false;
        // the table is a binary tree of entries at dword offsets
        std::vector<uint32_t> todo{0};
        std::string want = Lower(name);
        while (!todo.empty()) {
            uint32_t o = todo.back();
            todo.pop_back();
            if (o + 14 > t.size()) continue;
            uint16_t left = Le16(&t[o]), right = Le16(&t[o + 2]);
            if (left == 0xFFFF) continue;
            uint8_t len = t[o + 13];
            if (o + 14 + len > t.size()) continue;
            std::string n(reinterpret_cast<const char*>(&t[o + 14]), len);
            if (Lower(n) == want) {
                out_sector = Le32(&t[o + 4]);
                out_size = Le32(&t[o + 8]);
                dir = t[o + 12] & 0x10;
                return true;
            }
            if (left) todo.push_back(uint32_t(left) * 4);
            if (right) todo.push_back(uint32_t(right) * 4);
        }
        return false;
    }

    bool ReadFile(const std::string& rel, std::vector<uint8_t>& out, size_t limit) {
        std::vector<uint8_t> vd;
        if (!Read(32 * kSector, 0x20, vd)) return false;
        uint32_t sector = Le32(&vd[20]), size = Le32(&vd[24]);
        std::string path = rel;
        std::replace(path.begin(), path.end(), '\\', '/');
        size_t at = 0;
        while (true) {
            size_t slash = path.find('/', at);
            std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
            bool dir = false;
            if (!Find(sector, size, part, sector, size, dir)) return false;
            if (slash == std::string::npos) return !dir && Read(uint64_t(sector) * kSector, std::min<size_t>(size, limit), out);
            if (!dir) return false;
            at = slash + 1;
        }
    }
};

// ------------------------------------------------------------------ EA archives and images

// EA's RefPack (10 FB, then the unpacked size)
bool RefPack(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (in.size() < 5 || in[1] != 0xFB) return false;
    size_t p = 5 + ((in[0] & 0x01) ? 3 : 0);
    uint32_t size = uint32_t(in[2]) << 16 | uint32_t(in[3]) << 8 | in[4];
    out.clear();
    out.reserve(size);
    auto literal = [&](size_t n) {
        if (p + n > in.size()) return false;
        out.insert(out.end(), in.begin() + ptrdiff_t(p), in.begin() + ptrdiff_t(p + n));
        p += n;
        return true;
    };
    auto copy = [&](size_t n, size_t off) {
        if (off > out.size()) return false;
        for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - off]);
        return true;
    };
    while (p < in.size()) {
        uint8_t b0 = in[p];
        if (b0 < 0x80) {
            if (p + 2 > in.size()) return false;
            uint8_t b1 = in[p + 1];
            p += 2;
            if (!literal(b0 & 3) || !copy(((b0 & 0x1C) >> 2) + 3, ((b0 & 0x60) << 3) + b1 + 1)) return false;
        } else if (b0 < 0xC0) {
            if (p + 3 > in.size()) return false;
            uint8_t b1 = in[p + 1], b2 = in[p + 2];
            p += 3;
            if (!literal(b1 >> 6) || !copy((b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1)) return false;
        } else if (b0 < 0xE0) {
            if (p + 4 > in.size()) return false;
            uint8_t b1 = in[p + 1], b2 = in[p + 2], b3 = in[p + 3];
            p += 4;
            if (!literal(b0 & 3) || !copy(((b0 & 0x0C) << 6) + b3 + 5, ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1))
                return false;
        } else if (b0 < 0xFC) {
            p++;
            if (!literal(((b0 & 0x1F) << 2) + 4)) return false;
        } else {
            p++;
            literal(b0 & 3);
            break;
        }
    }
    return true;
}

void Unpack(std::vector<uint8_t>& v) {
    std::vector<uint8_t> out;
    if (v.size() > 2 && v[0] == 0x10 && v[1] == 0xFB && RefPack(v, out)) v.swap(out);
}

// BIGF / BIG4: name -> bytes
bool BigEntry(const std::vector<uint8_t>& d, const std::string& name, std::vector<uint8_t>& out) {
    if (d.size() < 16 || (std::memcmp(d.data(), "BIGF", 4) && std::memcmp(d.data(), "BIG4", 4))) return false;
    uint32_t n = Be32(&d[8]);
    size_t o = 16;
    std::string want = Lower(name);
    for (uint32_t i = 0; i < n && o + 8 < d.size(); i++) {
        uint32_t off = Be32(&d[o]), len = Be32(&d[o + 4]);
        o += 8;
        size_t e = o;
        while (e < d.size() && d[e]) e++;
        std::string entry(reinterpret_cast<const char*>(&d[o]), e - o);
        o = e + 1;
        if (Lower(entry) == want && size_t(off) + len <= d.size()) {
            out.assign(d.begin() + off, d.begin() + off + len);
            return true;
        }
    }
    return false;
}

// the first entry of a BIG whose name ends in .xsh, .fsh or .ssh
bool BigFirstImage(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
    if (d.size() < 16) return false;
    uint32_t n = Be32(&d[8]);
    size_t o = 16;
    for (uint32_t i = 0; i < n && o + 8 < d.size(); i++) {
        uint32_t off = Be32(&d[o]), len = Be32(&d[o + 4]);
        o += 8;
        size_t e = o;
        while (e < d.size() && d[e]) e++;
        std::string entry = Lower(std::string(reinterpret_cast<const char*>(&d[o]), e - o));
        o = e + 1;
        if (entry.size() > 4 && (entry.ends_with(".xsh") || entry.ends_with(".fsh") || entry.ends_with(".ssh")) &&
            size_t(off) + len <= d.size()) {
            out.assign(d.begin() + off, d.begin() + off + len);
            return true;
        }
    }
    return false;
}

void Rgb565(uint16_t v, uint8_t* c) {
    c[0] = uint8_t((v >> 11 & 31) * 255 / 31);
    c[1] = uint8_t((v >> 5 & 63) * 255 / 63);
    c[2] = uint8_t((v & 31) * 255 / 31);
}

// DXT1, DXT3 or DXT5 blocks into RGBA
bool Dxt(const uint8_t* p, size_t avail, int w, int h, int kind, std::vector<uint8_t>& rgba) {
    size_t block = kind == 1 ? 8 : 16;
    if (size_t((w + 3) / 4) * size_t((h + 3) / 4) * block > avail) return false;
    rgba.assign(size_t(w) * h * 4, 255);
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            uint8_t alpha[16];
            std::fill(std::begin(alpha), std::end(alpha), uint8_t(255));
            if (kind == 3) {
                for (int i = 0; i < 16; i++) alpha[i] = uint8_t(((p[i / 2] >> ((i & 1) * 4)) & 15) * 17);
                p += 8;
            } else if (kind == 5) {
                uint8_t a0 = p[0], a1 = p[1], al[8] = {a0, a1};
                if (a0 > a1)
                    for (int i = 0; i < 6; i++) al[2 + i] = uint8_t(((6 - i) * a0 + (i + 1) * a1) / 7);
                else {
                    for (int i = 0; i < 4; i++) al[2 + i] = uint8_t(((4 - i) * a0 + (i + 1) * a1) / 5);
                    al[6] = 0;
                    al[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(p[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = al[(bits >> (3 * i)) & 7];
                p += 8;
            }
            uint16_t c0 = Le16(p), c1 = Le16(p + 2);
            uint32_t bits = Le32(p + 4);
            p += 8;
            uint8_t col[4][3];
            Rgb565(c0, col[0]);
            Rgb565(c1, col[1]);
            bool four = c0 > c1 || kind != 1;
            for (int k = 0; k < 3; k++) {
                col[2][k] = uint8_t(four ? (2 * col[0][k] + col[1][k]) / 3 : (col[0][k] + col[1][k]) / 2);
                col[3][k] = uint8_t(four ? (col[0][k] + 2 * col[1][k]) / 3 : 0);
            }
            for (int i = 0; i < 16; i++) {
                int x = bx + i % 4, y = by + i / 4;
                if (x >= w || y >= h) continue;
                uint8_t* d = &rgba[(size_t(y) * w + x) * 4];
                const uint8_t* c = col[(bits >> (2 * i)) & 3];
                d[0] = c[0];
                d[1] = c[1];
                d[2] = c[2];
                d[3] = alpha[i];
            }
        }
    return true;
}

// the first image of an EA SHPX/SHPI file: DXT1 (60), DXT3 (61), DXT5 (62) or ARGB8888 (7D)
bool Shpx(const std::vector<uint8_t>& sh, ArtImage& img) {
    if (sh.size() < 24 || (std::memcmp(sh.data(), "SHPX", 4) && std::memcmp(sh.data(), "SHPI", 4))) return false;
    uint32_t off = Le32(&sh[20]);
    if (off + 16 > sh.size()) return false;
    uint8_t type = sh[off];
    int w = Le16(&sh[off + 4]), h = Le16(&sh[off + 6]);
    const uint8_t* p = &sh[off + 16];
    size_t avail = sh.size() - off - 16;
    img.w = w;
    img.h = h;
    if (type == 0x60) return Dxt(p, avail, w, h, 1, img.rgba);
    if (type == 0x61) return Dxt(p, avail, w, h, 3, img.rgba);
    if (type == 0x62) return Dxt(p, avail, w, h, 5, img.rgba);
    if (type == 0x7D && avail >= size_t(w) * h * 4) {
        img.rgba.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; i++) {  // BGRA in the file
            img.rgba[i * 4 + 0] = p[i * 4 + 2];
            img.rgba[i * 4 + 1] = p[i * 4 + 1];
            img.rgba[i * 4 + 2] = p[i * 4 + 0];
            img.rgba[i * 4 + 3] = p[i * 4 + 3];
        }
        return true;
    }
    return false;
}

// the dark margins the game's UVs never show, cut off
void Trim(ArtImage& img) {
    auto lum = [&](int x, int y) {
        const uint8_t* p = &img.rgba[(size_t(y) * img.w + x) * 4];
        return (p[0] * 3 + p[1] * 6 + p[2]) / 10;
    };
    auto col_dark = [&](int x) {
        long s = 0;
        for (int y = 0; y < img.h; y += 2) s += lum(x, y);
        return s / (img.h / 2 + 1) < 14;
    };
    auto row_dark = [&](int y, int x0, int x1) {
        long s = 0;
        for (int x = x0; x < x1; x += 2) s += lum(x, y);
        return s / ((x1 - x0) / 2 + 1) < 14;
    };
    int x0 = 0, x1 = img.w, y0 = 0, y1 = img.h;
    while (x1 - x0 > img.w / 3 && col_dark(x1 - 1)) x1--;
    while (x1 - x0 > img.w / 3 && col_dark(x0)) x0++;
    while (y1 - y0 > img.h / 3 && row_dark(y1 - 1, x0, x1)) y1--;
    while (y1 - y0 > img.h / 3 && row_dark(y0, x0, x1)) y0++;
    if (x0 == 0 && y0 == 0 && x1 == img.w && y1 == img.h) return;
    std::vector<uint8_t> out(size_t(x1 - x0) * (y1 - y0) * 4);
    for (int y = y0; y < y1; y++)
        std::memcpy(&out[size_t(y - y0) * (x1 - x0) * 4], &img.rgba[(size_t(y) * img.w + x0) * 4], size_t(x1 - x0) * 4);
    img.rgba.swap(out);
    img.w = x1 - x0;
    img.h = y1 - y0;
}

}  // namespace

bool ReadGameFile(const fs::path& game, const std::string& rel, std::vector<uint8_t>& out, size_t limit) {
    std::error_code ec;
    if (fs::is_directory(game, ec)) return ReadAll(game / FromU8(rel), out, limit);
    Xiso iso;
    return iso.Open(game) && iso.ReadFile(rel, out, limit);
}

bool DecodeImage(const std::vector<uint8_t>& bytes, ArtImage& out) {
    if (bytes.empty()) return false;
    int w, h, n;
    uint8_t* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return false;
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool ReadImageFile(const fs::path& file, ArtImage& out) {
    std::vector<uint8_t> bytes;
    if (!ReadAll(file, bytes) || !DecodeImage(bytes, out)) return false;
    out.name = U8(file.filename());
    return true;
}

bool XbeTitleImage(const std::vector<uint8_t>& xbe, ArtImage& out) {
    if (xbe.size() < 0x180 || std::memcmp(xbe.data(), "XBEH", 4) != 0) return false;
    uint32_t base = Le32(&xbe[0x104]), count = Le32(&xbe[0x11C]), headers = Le32(&xbe[0x120]) - base;
    for (uint32_t i = 0; i < count; i++) {
        size_t h = size_t(headers) + size_t(i) * 56;
        if (h + 56 > xbe.size()) return false;
        uint32_t raw = Le32(&xbe[h + 12]), size = Le32(&xbe[h + 16]), name = Le32(&xbe[h + 20]) - base;
        if (name + 10 > xbe.size() || std::memcmp(&xbe[name], "$$XTIMAGE", 10) != 0) continue;
        if (size_t(raw) + size > xbe.size() || size < 32) return false;
        // XPR0: the resource's header (common, data, lock, format, size) after the file's, its texels after the headers
        const uint8_t* x = &xbe[raw];
        if (std::memcmp(x, "XPR0", 4) != 0) return false;
        uint32_t header = Le32(x + 8), data = Le32(x + 16), format = Le32(x + 24);
        int kind = (format >> 8 & 0xFF) == 0x0C ? 1 : (format >> 8 & 0xFF) == 0x0E ? 3 : (format >> 8 & 0xFF) == 0x0F ? 5 : 0;
        int w = 1 << (format >> 20 & 15), hgt = 1 << (format >> 24 & 15);
        if (!kind || size_t(header) + data >= size) return false;
        out.name = "$$XTIMAGE";
        out.w = w;
        out.h = hgt;
        return Dxt(x + header + data, size - header - data, w, hgt, kind, out.rgba);
    }
    return false;
}

std::vector<ArtImage> LoadWebArt(const ArtSpec& spec, const fs::path& cache) {
    std::vector<ArtImage> out;
    std::error_code ec;
    fs::create_directories(cache, ec);
    for (size_t i = 0; i < spec.web.size(); i++) {
        const std::string& url = spec.web[i];
        std::string ext = Lower(url.substr(url.size() > 4 ? url.size() - 4 : 0));
        fs::path file = cache / ("web" + std::to_string(i) + (ext == ".png" ? ".png" : ".jpg"));
        if (!fs::is_regular_file(file, ec) && !DownloadPicture(url, file)) continue;
        ArtImage img;
        if (ReadImageFile(file, img)) out.push_back(std::move(img));
    }
    return out;
}

ArtSpec ReadArtSpec(const fs::path& profile_file) {
    ArtSpec s;
    toml::table t;
    try {
        t = toml::parse_file(U8(profile_file));
    } catch (...) {
        return s;
    }
    const toml::table* a = t["art"].as_table();
    if (!a) return s;
    s.kind = (*a)["kind"].value_or(std::string());
    s.file = (*a)["file"].value_or(std::string());
    s.seconds = float((*a)["seconds"].value_or(10.0));
    s.trim = (*a)["trim"].value_or(true);
    s.card_only = int((*a)["card_only"].value_or(int64_t(0)));
    if (const toml::array* w = (*a)["web"].as_array())
        for (auto& e : *w)
            if (auto v = e.value<std::string>()) s.web.push_back(*v);
    if (auto f = (*a)["folder"].value<std::string>()) {
        fs::path p = FromU8(*f);
        s.folder = (p.is_absolute() ? p : profile_file.parent_path() / p).lexically_normal().make_preferred();
    }
    if (const toml::array* inc = (*a)["include"].as_array())
        for (auto& e : *inc)
            if (auto v = e.value<std::string>()) s.include.push_back(*v);
    return s;
}

std::vector<ArtImage> LoadArt(const ArtSpec& spec, const fs::path& game) {
    std::vector<ArtImage> out;
    if (spec.Empty() || game.empty()) return out;
    if (spec.kind == "ea-viv") {
        std::vector<uint8_t> viv;
        if (!ReadGameFile(game, spec.file, viv)) return out;
        for (const std::string& name : spec.include) {
            std::vector<uint8_t> big, sh;
            if (!BigEntry(viv, name + ".big", big)) continue;
            Unpack(big);
            if (!BigFirstImage(big, sh)) continue;
            Unpack(sh);
            ArtImage img;
            img.name = name;
            if (!Shpx(sh, img)) continue;
            if (spec.trim) Trim(img);
            out.push_back(std::move(img));
        }
    } else if (spec.kind == "image") {
        for (const std::string& rel : spec.include) {
            std::vector<uint8_t> bytes;
            if (!ReadGameFile(game, rel, bytes)) continue;
            int w, h, n;
            uint8_t* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
            if (!px) continue;
            ArtImage img;
            img.name = rel;
            img.w = w;
            img.h = h;
            img.rgba.assign(px, px + size_t(w) * h * 4);
            stbi_image_free(px);
            if (spec.trim) Trim(img);
            out.push_back(std::move(img));
        }
    }
    return out;
}

}  // namespace launcher

#include "icon.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>

#include <stb_image.h>

#include "closegame.h"
#include "net.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace launcher {

namespace {

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void Put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; i++) v[at + i] = uint8_t(x >> (8 * i));
}
void Put16(std::vector<uint8_t>& v, size_t at, uint16_t x) {
    v[at] = uint8_t(x);
    v[at + 1] = uint8_t(x >> 8);
}

bool ReadBytes(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// a zip's entry, stored or deflated
bool ZipEntry(const std::vector<uint8_t>& z, const std::string& name, std::vector<uint8_t>& out) {
    if (z.size() < 22) return false;
    // the end of the central directory: its signature, within the last 64 KiB (the comment's longest)
    size_t end = SIZE_MAX;
    for (size_t i = z.size() - 22; z.size() - i <= 22 + 65535; i--) {
        if (Le32(&z[i]) == 0x06054b50) {
            end = i;
            break;
        }
        if (i == 0) break;
    }
    if (end == SIZE_MAX) return false;
    uint32_t count = Le16(&z[end + 10]);
    size_t p = Le32(&z[end + 16]);
    for (uint32_t i = 0; i < count && p + 46 <= z.size(); i++) {
        if (Le32(&z[p]) != 0x02014b50) return false;
        uint16_t method = Le16(&z[p + 10]), nlen = Le16(&z[p + 28]), xlen = Le16(&z[p + 30]), clen = Le16(&z[p + 32]);
        uint32_t csize = Le32(&z[p + 20]), usize = Le32(&z[p + 24]), local = Le32(&z[p + 42]);
        if (p + 46 + nlen > z.size()) return false;
        std::string n(reinterpret_cast<const char*>(&z[p + 46]), nlen);
        p += size_t(46) + nlen + xlen + clen;
        if (n != name) continue;
        if (size_t(local) + 30 > z.size()) return false;
        size_t data = size_t(local) + 30 + Le16(&z[local + 26]) + Le16(&z[local + 28]);
        if (data + csize > z.size() || usize > (64u << 20)) return false;
        out.resize(usize);
        if (method == 0 && csize == usize) {
            std::memcpy(out.data(), &z[data], usize);
            return true;
        }
        return method == 8 && stbi_zlib_decode_noheader_buffer(reinterpret_cast<char*>(out.data()), int(usize),
                                                               reinterpret_cast<const char*>(&z[data]), int(csize)) == int(usize);
    }
    return false;
}

std::string Trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    return s.substr(a);
}

// a J2ME jar's icon: its manifest's MIDlet-Icon, else the picture of its first MIDlet; game is the jar or its folder
bool JarIcon(const fs::path& game, ArtImage& out) {
    fs::path jar = game;
    std::error_code ec;
    if (fs::is_directory(game, ec)) {
        jar.clear();
        for (auto& e : fs::directory_iterator(game, ec)) {
            std::string ext = U8(e.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext == ".jar" && e.is_regular_file(ec)) {
                jar = e.path();
                break;
            }
        }
    }
    std::vector<uint8_t> z, mf, png;
    if (jar.empty() || !ReadBytes(jar, z) || !ZipEntry(z, "META-INF/MANIFEST.MF", mf)) return false;
    // a line that starts with a space carries on the one before
    std::vector<std::string> lines;
    std::string line;
    for (char c : mf) {
        if (c == '\r') continue;
        if (c != '\n') {
            line += c;
            continue;
        }
        if (!line.empty() && line[0] == ' ' && !lines.empty()) lines.back() += line.substr(1);
        else lines.push_back(line);
        line.clear();
    }
    if (!line.empty()) lines.push_back(line);
    std::string icon, first;
    for (const std::string& l : lines) {
        size_t colon = l.find(':');
        if (colon == std::string::npos) continue;
        std::string key = l.substr(0, colon), value = Trimmed(l.substr(colon + 1));
        if (key == "MIDlet-Icon") icon = value;
        // MIDlet-1: name, icon, class
        if (key == "MIDlet-1" && value.find(',') != std::string::npos) {
            std::string rest = value.substr(value.find(',') + 1);
            first = Trimmed(rest.substr(0, rest.find(',')));
        }
    }
    if (icon.empty()) icon = first;
    while (!icon.empty() && icon[0] == '/') icon.erase(0, 1);
    return !icon.empty() && ZipEntry(z, icon, png) && DecodeImage(png, out);
}

// the picture on a square of its longer side, centred on transparency
ArtImage Square(const ArtImage& in) {
    int n = std::max(in.w, in.h);
    if (in.w == in.h) return in;
    ArtImage out;
    out.name = in.name;
    out.w = out.h = n;
    out.rgba.assign(size_t(n) * n * 4, 0);
    int ox = (n - in.w) / 2, oy = (n - in.h) / 2;
    for (int y = 0; y < in.h; y++)
        std::memcpy(&out.rgba[(size_t(y + oy) * n + ox) * 4], &in.rgba[size_t(y) * in.w * 4], size_t(in.w) * 4);
    return out;
}

// each output sample's source samples and weights: the area each covers when shrinking, linear when growing
std::vector<std::vector<std::pair<int, float>>> Weights(int n, int m) {
    std::vector<std::vector<std::pair<int, float>>> w(static_cast<size_t>(m));
    float scale = float(n) / float(m);
    for (int i = 0; i < m; i++) {
        if (scale >= 1) {
            float a = float(i) * scale, b = a + scale;
            for (int j = int(a); j < n && float(j) < b; j++) {
                float cover = std::min(b, float(j) + 1) - std::max(a, float(j));
                if (cover > 0) w[size_t(i)].push_back({j, cover / scale});
            }
        } else {
            float c = (float(i) + 0.5f) * scale - 0.5f;
            int j = int(std::floor(c));
            float f = c - float(j);
            w[size_t(i)].push_back({std::clamp(j, 0, n - 1), 1 - f});
            w[size_t(i)].push_back({std::clamp(j + 1, 0, n - 1), f});
        }
    }
    return w;
}

// the square picture at size x size: pixel art by its nearest texel, the rest filtered on premultiplied colour (the
// transparent edges stay clean)
std::vector<uint8_t> Scaled(const ArtImage& sq, int size, bool pixel) {
    std::vector<uint8_t> out(size_t(size) * size * 4);
    if (pixel) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                std::memcpy(&out[(size_t(y) * size + x) * 4], &sq.rgba[(size_t(y * sq.h / size) * sq.w + x * sq.w / size) * 4], 4);
        return out;
    }
    std::vector<float> pre(sq.rgba.size());
    for (size_t i = 0; i < sq.rgba.size(); i += 4) {
        float a = sq.rgba[i + 3] / 255.f;
        for (int k = 0; k < 3; k++) pre[i + k] = sq.rgba[i + k] * a;
        pre[i + 3] = sq.rgba[i + 3];
    }
    auto wx = Weights(sq.w, size), wy = Weights(sq.h, size);
    std::vector<float> rows(size_t(size) * sq.h * 4, 0);
    for (int y = 0; y < sq.h; y++)
        for (int x = 0; x < size; x++)
            for (auto [j, f] : wx[size_t(x)])
                for (int k = 0; k < 4; k++) rows[(size_t(y) * size + x) * 4 + k] += pre[(size_t(y) * sq.w + j) * 4 + k] * f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float c[4] = {};
            for (auto [j, f] : wy[size_t(y)])
                for (int k = 0; k < 4; k++) c[k] += rows[(size_t(j) * size + x) * 4 + k] * f;
            uint8_t* d = &out[(size_t(y) * size + x) * 4];
            float a = std::clamp(c[3], 0.f, 255.f);
            for (int k = 0; k < 3; k++) d[k] = uint8_t(a > 0 ? std::clamp(c[k] * 255.f / a + 0.5f, 0.f, 255.f) : 0);
            d[3] = uint8_t(a + 0.5f);
        }
    return out;
}

// an .ico of the sizes Windows asks for (shortcut, desktop, Explorer), 32-bit BMP entries; written only when it changes
bool WriteIco(const fs::path& file, const ArtImage& sq, bool pixel) {
    std::vector<int> sizes;
    for (int s : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256})
        if (pixel || s <= std::max(sq.w, 48)) sizes.push_back(s);
    std::vector<uint8_t> out(6 + 16 * sizes.size(), 0);
    Put16(out, 2, 1);
    Put16(out, 4, uint16_t(sizes.size()));
    for (size_t i = 0; i < sizes.size(); i++) {
        int s = sizes[i];
        std::vector<uint8_t> px = Scaled(sq, s, pixel);
        size_t mask_row = size_t((s + 31) / 32) * 4, offset = out.size();
        size_t bytes = 40 + size_t(s) * s * 4 + mask_row * s;
        out.resize(offset + bytes, 0);
        // BITMAPINFOHEADER: the height counts the AND mask too; rows bottom-up, BGRA
        Put32(out, offset, 40);
        Put32(out, offset + 4, uint32_t(s));
        Put32(out, offset + 8, uint32_t(2 * s));
        Put16(out, offset + 12, 1);
        Put16(out, offset + 14, 32);
        Put32(out, offset + 20, uint32_t(bytes - 40));
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                const uint8_t* c = &px[(size_t(s - 1 - y) * s + x) * 4];
                uint8_t* d = &out[offset + 40 + (size_t(y) * s + x) * 4];
                d[0] = c[2];
                d[1] = c[1];
                d[2] = c[0];
                d[3] = c[3];
            }
        size_t e = 6 + 16 * i;
        out[e] = out[e + 1] = uint8_t(s >= 256 ? 0 : s);
        Put16(out, e + 4, 1);
        Put16(out, e + 6, 32);
        Put32(out, e + 8, uint32_t(bytes));
        Put32(out, e + 12, uint32_t(offset));
    }
    std::vector<uint8_t> old;
    if (ReadBytes(file, old) && old == out) return true;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o || !o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

}  // namespace

IconSpec ReadIconSpec(const fs::path& profile_file) {
    IconSpec s;
    toml::table t;
    try {
        t = toml::parse_file(U8(profile_file));
    } catch (...) {
        return s;
    }
    const toml::table* a = t["icon"].as_table();
    if (!a) return s;
    s.kind = (*a)["kind"].value_or(std::string("image"));
    s.pixel = (*a)["pixel"].value_or(false);
    for (const char* key : {"files", "web"})
        if (const toml::array* arr = (*a)[key].as_array())
            for (auto& e : *arr)
                if (auto v = e.value<std::string>()) (key[0] == 'f' ? s.files : s.web).push_back(*v);
    return s;
}

void GameIcon::Load(const IconSpec& spec, const fs::path& game, const std::vector<std::string>& files,
                    const fs::path& cache, const fs::path& ico) {
    if (spec.Empty()) return;
    int load = ++loads_;
    std::thread([this, spec, game, files, cache, ico, load] {
        ArtImage img;
        bool got = spec.kind == "jar" && !game.empty() && JarIcon(game, img);
        for (size_t i = 0; i < files.size() && !got; i++) {
            fs::path p = FromU8(files[i]);
            std::vector<uint8_t> bytes;
            bool read = p.is_absolute() ? ReadBytes(p, bytes) : !game.empty() && ReadGameFile(game, files[i], bytes);
            if (read) got = spec.kind == "xbe" ? XbeTitleImage(bytes, img) : DecodeImage(bytes, img);
        }
        // nothing from the player's copy: the public picture, downloaded the first time
        for (size_t i = 0; i < spec.web.size() && !got; i++) {
            std::error_code ec;
            fs::create_directories(cache, ec);
            fs::path file = cache / ("icone-web" + std::to_string(i) + ".img");
            if (!fs::is_regular_file(file, ec) && !DownloadPicture(spec.web[i], file)) continue;
            got = ReadImageFile(file, img);
        }
        if (!got || img.w <= 0 || img.h <= 0) return;
        ArtImage sq = Square(img);
        bool made = WriteIco(ico, sq, spec.pixel);
        std::lock_guard lock(m_);
        if (load != loads_) return;
        square_ = std::move(sq);
        pixel_ = spec.pixel;
        if (made) ico_ = ico;
    }).detach();
}

fs::path GameIcon::Ico() {
    std::lock_guard lock(m_);
    return ico_;
}

void* GameIcon::Handle(int size) {
#ifdef _WIN32
    if (auto it = handles_.find(size); it != handles_.end()) return it->second;
    std::vector<uint8_t> px = Scaled(square_, size, pixel_);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* d = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i < px.size(); i += 4) {
        d[i] = px[i + 2];
        d[i + 1] = px[i + 1];
        d[i + 2] = px[i];
        d[i + 3] = px[i + 3];
    }
    std::vector<uint8_t> zeros(size_t((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    handles_[size] = icon;
    return icon;
#else
    (void)size;
    return nullptr;
#endif
}

void GameIcon::Apply(uint64_t pid) {
#ifdef _WIN32
    std::lock_guard lock(m_);
    if (square_.rgba.empty() || !pid) return;
    // a look twice a second: the game's windows come and go (a splash, then the game's)
    uint64_t now = GetTickCount64();
    if (now < next_look_) return;
    next_look_ = now + 500;
    for (void* v : GameWindows(pid)) {
        if (seen_.count(v)) continue;
        HWND w = static_cast<HWND>(v);
        DWORD_PTR own = 0;
        // a window busy loading answers later
        if (!SendMessageTimeoutW(w, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &own)) continue;
        seen_.insert(v);
        if (own) continue;  // the port's own icon
        UINT dpi = GetDpiForWindow(w);
        if (!dpi) dpi = 96;
        HICON large = static_cast<HICON>(Handle(GetSystemMetricsForDpi(SM_CXICON, dpi)));
        HICON little = static_cast<HICON>(Handle(GetSystemMetricsForDpi(SM_CXSMICON, dpi)));
        DWORD_PTR r = 0;
        if (large) SendMessageTimeoutW(w, WM_SETICON, ICON_BIG, LPARAM(large), SMTO_ABORTIFHUNG, 100, &r);
        if (little) SendMessageTimeoutW(w, WM_SETICON, ICON_SMALL, LPARAM(little), SMTO_ABORTIFHUNG, 100, &r);
    }
#else
    (void)pid;
#endif
}

void GameIcon::Release() {
    std::lock_guard lock(m_);
#ifdef _WIN32
    for (auto& [size, h] : handles_)
        if (h) DestroyIcon(static_cast<HICON>(h));
#endif
    handles_.clear();
    seen_.clear();
    next_look_ = 0;
}

}  // namespace launcher

#include "install.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <thread>

#include "art.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace launcher {

namespace {

// SHA-1, FIPS 180-1
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t block[64];
    size_t used = 0;
    uint64_t bits = 0;
    static uint32_t Rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    void Compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = Rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void Add(const uint8_t* p, size_t n) {
        bits += uint64_t(n) * 8;
        while (n--) {
            block[used++] = *p++;
            if (used == 64) Compress(), used = 0;
        }
    }
    std::string Hex() {
        uint64_t len = bits;
        uint8_t one = 0x80, zero = 0;
        Add(&one, 1);
        while (used != 56) Add(&zero, 1);
        for (int i = 7; i >= 0; i--) {
            uint8_t b = uint8_t(len >> (i * 8));
            Add(&b, 1);
        }
        char s[41];
        for (int i = 0; i < 5; i++) std::snprintf(s + i * 8, 9, "%08x", h[i]);
        return s;
    }
};

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Ext(const fs::path& p) { return Lower(U8(p.extension())); }

bool ReadWhole(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

uint64_t FolderBytes(const fs::path& d) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(d, ec)) {
        if (!e.is_regular_file(ec)) continue;
#ifdef _WIN32
        // a file still being written has its size in itself, not yet in its folder
        HANDLE h = CreateFileW(e.path().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size{};
        if (h != INVALID_HANDLE_VALUE) {
            if (GetFileSizeEx(h, &size)) n += uint64_t(size.QuadPart);
            CloseHandle(h);
            continue;
        }
#endif
        n += e.file_size(ec);
    }
    return n;
}

// the system's tar: Windows 10 and 11 carry libarchive's, which reads 7z, zip and rar
std::string Tar() {
#ifdef _WIN32
    char sys[512] = {};
    const char* root = SDL_getenv("SystemRoot");
    std::snprintf(sys, sizeof(sys), "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
    return sys;
#else
    return "bsdtar";
#endif
}

// disc images and game folders under a folder: an image, else a folder holding the profile's file
std::vector<fs::path> Candidates(const fs::path& dir, const InstallSpec& spec) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!spec.check.empty() && fs::is_regular_file(dir / FromU8(spec.check), ec)) out.push_back(dir);
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
        }
        const fs::path& p = it->path();
        if (it->is_regular_file(ec) && (IsDiscImage(p) || IsGameFile(p, spec))) out.push_back(p);
        else if (it->is_directory(ec) && !spec.check.empty() && fs::is_regular_file(p / FromU8(spec.check), ec)) out.push_back(p);
    }
    return out;
}

}  // namespace

std::string Sha1Hex(const uint8_t* data, size_t size) {
    Sha1 s;
    s.Add(data, size);
    return s.Hex();
}

bool IsArchive(const fs::path& p) {
    std::string e = Ext(p);
    return e == ".7z" || e == ".zip" || e == ".rar" || e == ".tar" || e == ".gz" || e == ".xz";
}

bool IsDiscImage(const fs::path& p) {
    std::string e = Ext(p);
    return e == ".iso" || e == ".xiso";
}

bool IsGameFile(const fs::path& p, const InstallSpec& spec) {
    std::string e = Ext(p);
    for (const std::string& f : spec.files)
        if (e == "." + Lower(f)) return true;
    return false;
}

void Installer::Start(const fs::path& source, const fs::path& dest, const InstallSpec& spec) {
    {
        std::lock_guard lock(m_);
        if (st_.stage == Stage::Reading || st_.stage == Stage::Extracting || st_.stage == Stage::Checking) return;
        st_ = {};
        st_.stage = Stage::Reading;
    }
    cancel_ = false;
    std::thread([this, source, dest, spec] { Run(source, dest, spec); }).detach();
}

Installer::Status Installer::Get() {
    std::lock_guard lock(m_);
    return st_;
}

void Installer::Cancel() { cancel_ = true; }

bool Installer::Busy() {
    Stage s = Get().stage;
    return s == Stage::Reading || s == Stage::Extracting || s == Stage::Checking;
}

void Installer::Run(fs::path source, fs::path dest, InstallSpec spec) {
    auto fail = [&](const std::string& why) {
        std::lock_guard lock(m_);
        st_.stage = Stage::Failed;
        st_.error = why;
    };
    std::error_code ec;
    fs::path search = source;
    fs::path work;  // the archive's unpacking folder, ours to clear
    bool archive = fs::is_regular_file(source, ec) && (IsArchive(source) || (spec.unpack_images && IsDiscImage(source)));
    if (archive) {
        // its size unpacked, from the listing: "-rw-r--r--  0 0 0 <size> <month> <day> <year|time> <name>"
        std::string tar = Tar();
        std::string src = U8(source);
        const char* list_args[] = {tar.c_str(), "-tvf", src.c_str(), nullptr};
        SDL_Process* p = SDL_CreateProcess(list_args, true);
        if (!p) return fail("tar");
        size_t got = 0;
        int code = -1;
        char* out = static_cast<char*>(SDL_ReadProcess(p, &got, &code));
        SDL_DestroyProcess(p);
        uint64_t total = 0;
        if (out) {
            std::istringstream lines(std::string(out, got));
            SDL_free(out);
            std::string line;
            while (std::getline(lines, line)) {
                std::istringstream f(line);
                std::string mode, links, owner, group, size;
                f >> mode >> links >> owner >> group >> size;
                if (!mode.empty() && mode[0] == '-') total += std::strtoull(size.c_str(), nullptr, 10);
            }
        }
        if (code != 0 || !total) return fail("arquivo compactado ilegível");
        fs::create_directories(dest, ec);
        auto space = fs::space(dest, ec);
        if (!ec && space.available < total + (512ull << 20)) {
            char b[96];
            std::snprintf(b, sizeof(b), "espaço: precisa de %.1f GB livres", double(total + (512ull << 20)) / (1u << 30));
            return fail(b);
        }
        {
            std::lock_guard lock(m_);
            st_.stage = Stage::Extracting;
            st_.from_archive = true;
            st_.archive = source;
            st_.archive_bytes = fs::file_size(source, ec);
        }
        work = dest / "_extraindo";
        fs::remove_all(work, ec);
        fs::create_directories(work, ec);
        std::string to = U8(work);
        const char* x_args[] = {tar.c_str(), "-xf", src.c_str(), "-C", to.c_str(), nullptr};
        p = SDL_CreateProcess(x_args, false);
        if (!p) return fail("tar");
        // the progress is what has landed so far
        while (!SDL_WaitProcess(p, false, &code)) {
            if (cancel_) {
                SDL_KillProcess(p, true);
                SDL_WaitProcess(p, true, &code);
                SDL_DestroyProcess(p);
                fs::remove_all(work, ec);
                return fail("cancelado");
            }
            SDL_Delay(400);
            float done = float(double(FolderBytes(work)) / double(total));
            std::lock_guard lock(m_);
            st_.progress = std::min(done, 0.999f);
        }
        SDL_DestroyProcess(p);
        if (code != 0) {
            fs::remove_all(work, ec);
            return fail("falha ao descompactar (tar " + std::to_string(code) + ")");
        }
        search = work;
    }
    {
        std::lock_guard lock(m_);
        st_.stage = Stage::Checking;
        st_.progress = 1;
    }
    std::vector<fs::path> found;
    if (fs::is_regular_file(search, ec)) found.push_back(search);
    else found = Candidates(search, spec);
    if (found.empty()) {
        if (!work.empty()) fs::remove_all(work, ec);
        return fail("nenhuma imagem do disco (.iso) ou pasta do jogo encontrada");
    }
    // the release the port was made from, else the first one found
    fs::path pick = found[0];
    bool ok = false;
    for (const fs::path& c : found) {
        std::vector<uint8_t> bytes;
        bool read = spec.check.empty() ? IsGameFile(c, spec) && ReadWhole(c, bytes) : ReadGameFile(c, spec.check, bytes);
        if (read && Lower(Sha1Hex(bytes.data(), bytes.size())) == Lower(spec.sha1)) {
            pick = c;
            ok = true;
            break;
        }
    }
    if (spec.sha1.empty()) ok = true;
    // a game of one file is copied into the port's folder: cleaning Downloads does not lose it
    if (work.empty() && IsGameFile(pick, spec)) {
        fs::create_directories(dest, ec);
        fs::path final = dest / pick.filename();
        if (fs::absolute(final, ec) != fs::absolute(pick, ec)) {
            fs::copy_file(pick, final, fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("não foi possível copiar o jogo: " + ec.message());
        }
        pick = final;
    }
    // an archive's game moves out of the unpacking folder into the port's; the rest of it goes
    if (!work.empty()) {
        // the archive's top is the game itself: its unpacking folder is kept under a name of its own
        fs::path final = dest / (fs::equivalent(pick, work, ec) ? fs::path("conteudo") : pick.filename());
        if (fs::exists(final, ec)) fs::remove_all(final, ec);
        fs::rename(pick, final, ec);
        if (ec) return fail("não foi possível mover o jogo: " + ec.message());
        fs::remove_all(work, ec);
        pick = final;
    }
    std::lock_guard lock(m_);
    st_.stage = Stage::Done;
    st_.game = pick;
    st_.version_ok = ok;
}

}  // namespace launcher

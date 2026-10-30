#include "install.h"
#include "i18n.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <thread>

#include "art.h"
#include "net.h"

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

// a program's output with its errors in it, and how it ended (-1: it did not start)
std::string RunOutput(const std::vector<std::string>& args, int& code) {
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    code = -1;
    if (!p) return {};
    size_t got = 0;
    char* out = static_cast<char*>(SDL_ReadProcess(p, &got, &code));
    SDL_DestroyProcess(p);
    std::string text = out ? std::string(out, got) : std::string();
    SDL_free(out);
    return text;
}

// the last line with something in it
std::string LastLine(const std::string& text) {
    std::string last, line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) last = line;
    }
    return last;
}

#ifdef _WIN32
// a PowerShell single-quoted string
std::string PsQuote(const std::string& s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("''") : std::string(1, c);
    return o + "'";
}

std::string PowerShell() {
    const char* root = SDL_getenv("SystemRoot");
    return std::string(root ? root : "C:\\Windows") + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
}

// a disc image mounted by Windows itself, read-only (it reads ISO 9660 and UDF images that tar does not): its root,
// such as "E:\", empty when it does not mount. An image the player had mounted already is used as it is
std::string MountImage(const fs::path& image, bool& ours) {
    std::string img = PsQuote(U8(image));
    std::string script = "$ErrorActionPreference='Stop'; $i=Get-DiskImage -ImagePath " + img + "; $ours=$false; " +
                         "if (-not $i.Attached) { $i=Mount-DiskImage -ImagePath " + img + " -Access ReadOnly -PassThru; $ours=$true }; " +
                         "$l=$null; for ($n=0; $n -lt 40 -and -not $l; $n++) { $l=($i | Get-Volume).DriveLetter; " +
                         "if (-not $l) { Start-Sleep -Milliseconds 250 } }; if ($l) { $l + ':' + $ours }";
    int code = 0;
    std::string out = LastLine(RunOutput({PowerShell(), "-NoProfile", "-NonInteractive", "-Command", script}, code));
    ours = out.size() > 2 && out.substr(2) == "True";
    if (code != 0 || out.size() < 2 || out[1] != ':' || !std::isalpha(static_cast<unsigned char>(out[0]))) return {};
    return out.substr(0, 1) + ":\\";
}

void UnmountImage(const fs::path& image) {
    int code = 0;
    RunOutput({PowerShell(), "-NoProfile", "-NonInteractive", "-Command",
               "Dismount-DiskImage -ImagePath " + PsQuote(U8(image)) + " | Out-Null"}, code);
}
#endif

// the .pkg files of a PS4 game: the one picked and its neighbours, or the ones in the folder picked (two folders down)
std::vector<fs::path> FindPkgs(const fs::path& where) {
    std::vector<fs::path> out;
    std::error_code ec;
    auto is_pkg = [](const fs::path& p) { return Ext(p) == ".pkg"; };
    if (fs::is_regular_file(where, ec)) {
        if (!is_pkg(where)) return out;
        for (auto& e : fs::directory_iterator(where.parent_path(), ec))
            if (e.is_regular_file(ec) && is_pkg(e.path())) out.push_back(e.path());
    } else if (fs::is_directory(where, ec)) {
        for (auto it = fs::recursive_directory_iterator(where, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 2) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && is_pkg(it->path())) out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// the PS4 .pkg extractor (a build of shadPS4's own package code), kept on the launcher's server: downloaded once, into
// the port's tools folder. Empty path and the reason when it cannot be had
constexpr const char* kPkgToolKey = "ferramentas/ps4-pkg-extractor.zip";
constexpr const char* kPkgToolSha256 = "5f5d475ad90418a8650ee1eff476e60192b0632e73af6e5107fa5bd00b94f02a";

fs::path PkgTool(const fs::path& tools, std::atomic<bool>& cancel, std::string& why) {
    std::error_code ec;
    fs::path exe = tools / "pkg_extractor.exe";
    if (fs::is_regular_file(exe, ec)) return exe;
    std::string url = ServerFile(kPkgToolKey);
    if (url.empty()) {
        why = L3("sem conexão com o servidor do launcher (a ferramenta que abre o .pkg vem de lá)", "no connection to the launcher's server (the tool that opens the .pkg comes from there)", "sin conexión con el servidor del launcher (la herramienta que abre el .pkg viene de allí)");
        return {};
    }
    fs::create_directories(tools, ec);
    fs::path zip = tools / "extrator.zip";
    uint64_t got = 0;
    std::mutex m;
    std::string sha, error;
    if (!DownloadFile(url, zip, cancel, got, m, sha, error) || sha != kPkgToolSha256) {
        fs::remove(zip, ec);
        why = L3("não deu para baixar a ferramenta que abre o .pkg", "could not download the tool that opens the .pkg", "no se pudo descargar la herramienta que abre el .pkg") + (error.empty() ? std::string() : " (" + error + ")");
        return {};
    }
    std::string tar = Tar(), src = U8(zip), to = U8(tools);
    const char* x_args[] = {tar.c_str(), "-xf", src.c_str(), "-C", to.c_str(), nullptr};
    SDL_Process* p = SDL_CreateProcess(x_args, false);
    int code = -1;
    if (p) SDL_WaitProcess(p, true, &code), SDL_DestroyProcess(p);
    fs::remove(zip, ec);
    if (code != 0 || !fs::is_regular_file(exe, ec)) {
        why = L3("não deu para abrir a ferramenta baixada", "could not open the downloaded tool", "no se pudo abrir la herramienta descargada");
        return {};
    }
    return exe;
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
    // a folder shown that holds the download itself and not the game (Downloads): the one archive or disc image in it
    // is what gets unpacked
    if (fs::is_directory(source, ec) && Candidates(source, spec).empty()) {
        fs::path only;
        int n = 0;
        for (auto& e : fs::directory_iterator(source, ec))
            if (e.is_regular_file(ec) && (IsArchive(e.path()) || (spec.unpack_images && IsDiscImage(e.path())))) only = e.path(), n++;
        if (n == 1) source = only, search = only;
    }
    fs::path work;  // the archive's unpacking folder, ours to clear
    bool archive = fs::is_regular_file(source, ec) && (IsArchive(source) || (spec.unpack_images && IsDiscImage(source)));
    if (archive) {
        // its size unpacked, from the listing: "-rw-r--r--  0 0 0 <size> <month> <day> <year|time> <name>"
        std::string tar = Tar();
        std::string src = U8(source);
        const char* list_args[] = {tar.c_str(), "-tvf", src.c_str(), nullptr};
        SDL_Process* lp = SDL_CreateProcess(list_args, true);
        if (!lp) return fail("tar");
        size_t got = 0;
        int code = -1;
        char* out = static_cast<char*>(SDL_ReadProcess(lp, &got, &code));
        SDL_DestroyProcess(lp);
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
        // a disc image tar does not read (report 84: a PS3 .iso): Windows mounts it and its files are copied out;
        // else tar's own words go in the message
        std::string mounted;
        bool mounted_here = false;
        // tests: LAUNCHER_ISO_MOUNT=1, a disc image goes the mounted way even when tar reads it
        bool force_mount = SDL_getenv("LAUNCHER_ISO_MOUNT") && IsDiscImage(source);
        if (code != 0 || !total || force_mount) {
#ifdef _WIN32
            if (IsDiscImage(source)) mounted = MountImage(source, mounted_here);
#endif
            if (mounted.empty()) {
                // tar's own words, from listing it again with its errors kept
                int again = 0;
                std::string why = code != 0 ? LastLine(RunOutput({tar, "-tvf", src}, again)) : std::string();
                return fail(L3("arquivo compactado ilegível", "unreadable archive", "archivo comprimido ilegible") + (why.empty() ? std::string() : " (" + why + ")"));
            }
            total = FolderBytes(FromU8(mounted));
        }
        auto unmount = [&] {
#ifdef _WIN32
            if (mounted_here) UnmountImage(source);
#endif
        };
        if (!total) {
            unmount();
            return fail(L3("a imagem do disco está vazia", "the disc image is empty", "la imagen del disco está vacía"));
        }
        fs::create_directories(dest, ec);
        auto space = fs::space(dest, ec);
        if (!ec && space.available < total + (512ull << 20)) {
            unmount();
            char b[96];
            std::snprintf(b, sizeof(b), L3("espaço: precisa de %.1f GB livres", "space: needs %.1f GB free", "espacio: necesita %.1f GB libres").c_str(), double(total + (512ull << 20)) / (1u << 30));
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
        // robocopy's codes below 8 are success (1: files copied)
        std::string robocopy = std::string(SDL_getenv("SystemRoot") ? SDL_getenv("SystemRoot") : "C:\\Windows") + "\\System32\\robocopy.exe";
        const char* x_args[] = {tar.c_str(), "-xf", src.c_str(), "-C", to.c_str(), nullptr};
        const char* copy_args[] = {robocopy.c_str(), mounted.c_str(), to.c_str(), "/E", "/R:1", "/W:1", "/NFL", "/NDL",
                                   "/NJH", "/NJS", "/NP", nullptr};
        SDL_Process* p = SDL_CreateProcess(mounted.empty() ? x_args : copy_args, false);
        if (!p) {
            unmount();
            return fail(mounted.empty() ? "tar" : "robocopy");
        }
        // the progress is what has landed so far
        while (!SDL_WaitProcess(p, false, &code)) {
            if (cancel_) {
                SDL_KillProcess(p, true);
                SDL_WaitProcess(p, true, &code);
                SDL_DestroyProcess(p);
                unmount();
                fs::remove_all(work, ec);
                return fail("cancelado");
            }
            SDL_Delay(400);
            float done = float(double(FolderBytes(work)) / double(total));
            std::lock_guard lock(m_);
            st_.progress = std::min(done, 0.999f);
        }
        SDL_DestroyProcess(p);
        unmount();
        if (!mounted.empty()) code = code < 8 ? 0 : code;
        if (code != 0) {
            fs::remove_all(work, ec);
            return fail((mounted.empty() ? "falha ao descompactar (tar " : "falha ao copiar a imagem montada (robocopy ") +
                        std::to_string(code) + ")");
        }
        search = work;
    }
    // a PS4 game given as .pkg files (base, patch, DLC): extracted beside each other in the port's folder, the way the
    // emulator looks for them (CUSA00004, CUSA00004-patch), unless the game's folder is there already
    if (!spec.pkg.empty() && !(fs::is_directory(search, ec) && !Candidates(search, spec).empty())) {
        std::vector<fs::path> pkgs = FindPkgs(search);
        if (!pkgs.empty()) {
            std::string why;
            fs::path tool = PkgTool(dest.parent_path() / "ferramentas", cancel_, why);
            if (tool.empty()) {
                if (!work.empty()) fs::remove_all(work, ec);
                return fail(why);
            }
            uint64_t total = 0;
            for (const fs::path& k : pkgs) total += fs::file_size(k, ec);
            fs::create_directories(dest, ec);
            auto space = fs::space(dest, ec);
            if (!ec && space.available < total + (512ull << 20)) {
                if (!work.empty()) fs::remove_all(work, ec);
                char b[96];
                std::snprintf(b, sizeof(b), L3("espaço: precisa de %.1f GB livres", "space: needs %.1f GB free", "espacio: necesita %.1f GB libres").c_str(), double(total + (512ull << 20)) / (1u << 30));
                return fail(b);
            }
            {
                std::lock_guard lock(m_);
                st_.stage = Stage::Extracting;
                st_.progress = 0;
                st_.from_archive = pkgs.size() == 1;
                st_.archive = pkgs[0];
                st_.archive_bytes = total;
            }
            uint64_t before = FolderBytes(dest);
            std::string exe = U8(tool);
            for (const fs::path& k : pkgs) {
                std::string file = U8(k);
                // its kind: 101 the game, 102 a patch, 103 an add-on (kept apart: it takes the game's folder name)
                const char* c_args[] = {exe.c_str(), file.c_str(), "--check-type", nullptr};
                SDL_Process* cp = SDL_CreateProcess(c_args, false);
                int kind = -1;
                if (cp) SDL_WaitProcess(cp, true, &kind), SDL_DestroyProcess(cp);
                if (kind != 101 && kind != 102 && kind != 103) {
                    if (!work.empty()) fs::remove_all(work, ec);
                    return fail(L3("o arquivo ", "the file ", "el archivo ") + U8(k.filename()) + L3(" não parece um .pkg de jogo do PS4", " does not look like a PS4 game .pkg", " no parece un .pkg de juego de PS4"));
                }
                fs::path out = kind == 103 ? dest / "_dlc" : dest;
                fs::create_directories(out, ec);
                std::string o = U8(out);
                const char* x_args[] = {exe.c_str(), file.c_str(), o.c_str(), nullptr};
                SDL_Process* p = SDL_CreateProcess(x_args, false);
                if (!p) return fail("pkg_extractor");
                int code = -1;
                while (!SDL_WaitProcess(p, false, &code)) {
                    if (cancel_) {
                        SDL_KillProcess(p, true);
                        SDL_WaitProcess(p, true, &code);
                        SDL_DestroyProcess(p);
                        return fail("cancelado");
                    }
                    SDL_Delay(400);
                    uint64_t now = FolderBytes(dest);
                    std::lock_guard lock(m_);
                    st_.progress = std::min(float(double(now > before ? now - before : 0) / double(std::max<uint64_t>(total, 1))), 0.999f);
                }
                SDL_DestroyProcess(p);
                if (code != 0) return fail(L3("falha ao extrair ", "failed to extract ", "falló al extraer ") + U8(k.filename()) + L3(" (código ", " (code ", " (código ") + std::to_string(code) + ")");
            }
            if (!work.empty()) fs::remove_all(work, ec);
            fs::path game = dest / spec.pkg;
            if (!fs::is_regular_file(game / "eboot.bin", ec)) return fail(L3("faltou o .pkg do jogo em si (o maior, com o jogo base ", "the game's own .pkg is missing (the largest, with the base game ", "falta el .pkg del juego en sí (el mayor, con el juego base ") + spec.pkg + L3("); mostre a pasta que tem todos os .pkg", "); show the folder that has all the .pkg files", "); muestra la carpeta que tiene todos los .pkg"));
            std::lock_guard lock(m_);
            st_.stage = Stage::Done;
            st_.game = game;
            st_.version_ok = true;
            return;
        }
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
        return fail(L3("nenhuma imagem do disco (.iso) ou pasta do jogo encontrada", "no disc image (.iso) or game folder found", "no se encontró ninguna imagen del disco (.iso) ni carpeta del juego"));
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
            if (ec) return fail(L3("não foi possível copiar o jogo: ", "could not copy the game: ", "no se pudo copiar el juego: ") + ec.message());
        }
        pick = final;
    }
    // an archive's game moves out of the unpacking folder into the port's; the rest of it goes
    if (!work.empty()) {
        // the archive's top is the game itself: its unpacking folder is kept under a name of its own
        fs::path final = dest / (fs::equivalent(pick, work, ec) ? fs::path("conteudo") : pick.filename());
        if (fs::exists(final, ec)) fs::remove_all(final, ec);
        fs::rename(pick, final, ec);
        if (ec) return fail(L3("não foi possível mover o jogo: ", "could not move the game: ", "no se pudo mover el juego: ") + ec.message());
        fs::remove_all(work, ec);
        pick = final;
    }
    std::lock_guard lock(m_);
    st_.stage = Stage::Done;
    st_.game = pick;
    st_.version_ok = ok;
}

}  // namespace launcher

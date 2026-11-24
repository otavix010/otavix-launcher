#include "unpack.h"
#include "i18n.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace unpack {

using launcher::L3;

namespace {

// a program's exit code, -1 when it did not even start (why = SDL's reason)
int Run(const std::vector<std::string>& args, std::string& why) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    SDL_Process* p = SDL_CreateProcess(argv.data(), false);
    if (!p) {
        why = SDL_GetError();
        return -1;
    }
    int code = -1;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    return code;
}

#ifndef _WIN32
// a program found on PATH, as the shell would run it
bool OnPath(const char* name) {
    const char* path = SDL_getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        std::string dir = dirs.substr(start, end - start);
        if (!dir.empty()) {
            std::error_code ec;
            fs::path p = fs::path(dir) / name;
            if (fs::is_regular_file(p, ec) && (fs::status(p, ec).permissions() & fs::perms::others_exec) != fs::perms::none)
                return true;
        }
        start = end + 1;
    }
    return false;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool EndsWith(const std::string& s, const char* tail) {
    size_t n = std::char_traits<char>::length(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}
#endif

}  // namespace

std::string Tar() {
#ifdef _WIN32
    char sys[512] = {};
    const char* root = SDL_getenv("SystemRoot");
    std::snprintf(sys, sizeof(sys), "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
    return sys;
#else
    return OnPath("bsdtar") ? "bsdtar" : "";
#endif
}

bool PackTarGz(const std::string& parent, const std::vector<std::string>& names, const std::string& out, std::string& why) {
    std::string tar = Tar();
#ifndef _WIN32
    if (tar.empty()) tar = "tar";  // GNU tar makes .tar.gz too
#endif
    std::vector<std::string> args = {tar, "-czf", out, "-C", parent};
    for (const auto& n : names) args.push_back(n);
    std::string start_error;
    int code = Run(args, start_error);
    if (code == 0) return true;
    why = code < 0 ? start_error : "tar: " + std::to_string(code);
    return false;
}

std::string MissingBsdtarHint() {
#ifdef _WIN32
    return {};
#else
    return L3("instale o bsdtar: no Ubuntu/Debian/Mint o pacote libarchive-tools (sudo apt install libarchive-tools); no Fedora, Arch e SteamOS o pacote bsdtar/libarchive",
              "install bsdtar: on Ubuntu/Debian/Mint the libarchive-tools package (sudo apt install libarchive-tools); on Fedora, Arch and SteamOS the bsdtar/libarchive package",
              "instale bsdtar: en Ubuntu/Debian/Mint el paquete libarchive-tools (sudo apt install libarchive-tools); en Fedora, Arch y SteamOS el paquete bsdtar/libarchive");
#endif
}

bool Extract(const std::string& archive, const std::string& dest, std::string& why) {
    std::string start_error;
    const std::string tar = Tar();
    if (!tar.empty()) {
        int code = Run({tar, "-xf", archive, "-C", dest}, start_error);
        if (code == 0) return true;
        if (code != -1) {
            why = "tar " + L3("saiu com ", "exited with ", "salió con ") + std::to_string(code);
            return false;
        }
    }
#ifdef _WIN32
    why = L3("tar.exe não abriu: ", "tar.exe did not start: ", "tar.exe no se abrió: ") + start_error;
    return false;
#else
    // without bsdtar: GNU tar for the tar formats (keeps the permissions), unzip for a .zip
    const std::string name = Lower(archive);
    std::vector<std::string> other;
    const char* tool = nullptr;
    if (EndsWith(name, ".tar.gz") || EndsWith(name, ".tgz") || EndsWith(name, ".tar") || EndsWith(name, ".tar.xz") ||
        EndsWith(name, ".tar.bz2") || EndsWith(name, ".tar.zst")) {
        tool = "tar";
        other = {"tar", "-xf", archive, "-C", dest};
    } else if (EndsWith(name, ".zip")) {
        tool = "unzip";
        other = {"unzip", "-o", "-q", archive, "-d", dest};
    }
    if (tool && OnPath(tool)) {
        std::string err;
        int code = Run(other, err);
        if (code == 0) return true;
        why = std::string(tool) + (code == -1 ? L3(" não abriu: ", " did not start: ", " no se abrió: ") + err
                                              : L3(" saiu com ", " exited with ", " salió con ") + std::to_string(code));
        return false;
    }
    why = L3("o bsdtar não está instalado", "bsdtar is not installed", "bsdtar no está instalado") +
          (tool ? std::string(L3(" (nem o ", " (nor ", " (ni ")) + tool + ")" : std::string()) + "; " + MissingBsdtarHint();
    return false;
#endif
}

}  // namespace unpack

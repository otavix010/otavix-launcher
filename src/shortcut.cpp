#include "shortcut.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include <SDL3/SDL.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace launcher {

bool DesktopShortcut(const std::string& name, const std::string& args, const fs::path& icon, const fs::path& workdir,
                     std::string& error) {
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    // the file name: what Windows does not take in a name, out
    std::wstring file = FromU8(name).wstring();
    for (wchar_t& c : file)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L' ';
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) {
        error = "desktop";
        return false;
    }
    std::wstring lnk = std::wstring(desktop) + L"\\" + file + L".lnk";
    CoTaskMemFree(desktop);
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (SUCCEEDED(hr)) {
        link->SetPath(self);
        link->SetArguments(FromU8(args).wstring().c_str());
        link->SetWorkingDirectory(workdir.wstring().c_str());
        link->SetIconLocation(icon.empty() ? self : icon.wstring().c_str(), 0);
        IPersistFile* pf = nullptr;
        hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf));
        if (SUCCEEDED(hr)) {
            hr = pf->Save(lnk.c_str(), TRUE);
            pf->Release();
        }
        link->Release();
    }
    if (com) CoUninitialize();
    if (FAILED(hr)) {
        char b[32];
        std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
        error = b;
        return false;
    }
    return true;
#else
    // a .desktop entry in the applications menu (and on the desktop folder when there is one); on SteamOS also added to
    // the Steam library (steamos-add-to-steam), where it shows in Game Mode
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) {
        error = "exe";
        return false;
    }
    self[n] = 0;
    std::string file = name;
    for (char& c : file)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = ' ';
    const char* home = SDL_getenv("HOME");
    if (!home || !*home) {
        error = "HOME";
        return false;
    }
    std::string ic = U8(icon);
    bool image = ic.size() > 4 && (ic.compare(ic.size() - 4, 4, ".png") == 0 || ic.compare(ic.size() - 4, 4, ".svg") == 0);
    std::string text = "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=\"" + self + "\" " + args + "\nPath=" + U8(workdir) +
                       "\n" + (image ? "Icon=" + ic + "\n" : std::string()) + "Categories=Game;\nTerminal=false\n";
    std::error_code ec;
    fs::path apps = fs::path(home) / ".local" / "share" / "applications";
    fs::create_directories(apps, ec);
    fs::path entry = apps / (file + ".desktop");
    {
        std::ofstream out(entry, std::ios::binary | std::ios::trunc);
        if (!out || !(out << text)) {
            error = U8(entry);
            return false;
        }
    }
    fs::permissions(entry, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    if (fs::path desk = fs::path(home) / "Desktop"; fs::is_directory(desk, ec)) {
        fs::copy_file(entry, desk / entry.filename(), fs::copy_options::overwrite_existing, ec);
    }
    if (::access("/usr/bin/steamos-add-to-steam", X_OK) == 0) {
        std::string e = U8(entry);
        const char* argv[] = {"/usr/bin/steamos-add-to-steam", e.c_str(), nullptr};
        if (SDL_Process* p = SDL_CreateProcess(argv, false)) SDL_DestroyProcess(p);
    }
    return true;
#endif
}

}  // namespace launcher

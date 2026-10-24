#include "shortcut.h"

#include <cstdio>

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
    (void)name, (void)args, (void)icon, (void)workdir;
    error = "not on this system yet";
    return false;
#endif
}

}  // namespace launcher

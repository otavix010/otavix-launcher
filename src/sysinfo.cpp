#include "sysinfo.h"

#include <cstdio>
#include <fstream>

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#endif

namespace launcher {

namespace {

#ifdef _WIN32
std::string Narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string RegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[512]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return Narrow(buf);
}

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}
#endif

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
    size_t a = s.find_first_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a);
}

}  // namespace

std::vector<std::pair<std::string, std::string>> SystemInfo() {
    std::vector<std::pair<std::string, std::string>> out;
#ifdef _WIN32
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string build = RegString(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild");
    int b = std::atoi(build.c_str());
    // the registry still calls 11 "Windows 10"
    std::string os = b >= 22000 ? "Windows 11" : "Windows 10";
    std::string disp = RegString(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion");
    if (!disp.empty()) os += " " + disp;
    os += " (" + build + "." + std::to_string(RegDword(HKEY_LOCAL_MACHINE, nt, L"UBR")) + ")";
    out.push_back({"os", os});
    out.push_back({"cpu", Trim(RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                         L"ProcessorNameString"))});
    // the first hardware adapter, with its driver's version
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                char vram[64];
                std::snprintf(vram, sizeof(vram), " (%.0f GB)", double(d.DedicatedVideoMemory) / (1024.0 * 1024 * 1024));
                out.push_back({"gpu", Narrow(d.Description) + vram});
                LARGE_INTEGER v{};
                if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) {
                    char ver[64];
                    std::snprintf(ver, sizeof(ver), "%u.%u.%u.%u", unsigned(HIWORD(v.HighPart)), unsigned(LOWORD(v.HighPart)),
                                  unsigned(HIWORD(v.LowPart)), unsigned(LOWORD(v.LowPart)));
                    out.push_back({"driver", ver});
                }
                a->Release();
                break;
            }
            a->Release();
        }
        f->Release();
    }
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) {
        char ram[32];
        std::snprintf(ram, sizeof(ram), "%.0f GB", double(m.ullTotalPhys) / (1024.0 * 1024 * 1024));
        out.push_back({"ram", ram});
    }
#else
    out.push_back({"os", SDL_GetPlatform()});
#endif
    if (const SDL_DisplayMode* d = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        char s[64];
        std::snprintf(s, sizeof(s), "%d x %d @ %.0f Hz", int(d->w * d->pixel_density), int(d->h * d->pixel_density),
                      double(d->refresh_rate));
        out.push_back({"display", s});
    }
    return out;
}

fs::path NewestFile(const fs::path& folder) {
    std::error_code ec;
    fs::path best;
    fs::file_time_type when{};
    for (auto& e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto t = e.last_write_time(ec);
        if (best.empty() || t > when) {
            best = e.path();
            when = t;
        }
    }
    return best;
}

std::string FileTail(const fs::path& file, size_t max_bytes) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return {};
    auto size = size_t(in.tellg());
    size_t from = size > max_bytes ? size - max_bytes : 0;
    in.seekg(std::streamoff(from));
    std::string s(size - from, '\0');
    in.read(s.data(), std::streamsize(s.size()));
    if (from) {
        size_t nl = s.find('\n');
        if (nl != std::string::npos) s.erase(0, nl + 1);
    }
    return s;
}

std::string FileHeadTail(const fs::path& file, size_t head, size_t tail) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return {};
    auto size = size_t(in.tellg());
    if (size <= head + tail) return FileTail(file, size);
    in.seekg(0);
    std::string start(head, '\0');
    in.read(start.data(), std::streamsize(head));
    size_t nl = start.rfind('\n');
    if (nl != std::string::npos) start.erase(nl + 1);
    std::string end = FileTail(file, tail);
    return start + "\n[... " + std::to_string(size - start.size() - end.size()) + " bytes do meio do log cortados ...]\n\n" + end;
}

}  // namespace launcher

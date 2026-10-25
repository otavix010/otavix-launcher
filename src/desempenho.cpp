#include "desempenho.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "net.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <intrin.h>
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

std::string RegString(const wchar_t* key, const wchar_t* value) {
    wchar_t buf[512]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return Narrow(buf);
}

DWORD RegDword(const wchar_t* key, const wchar_t* value) {
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}
#endif

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    size_t a = s.find_first_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a);
}

// one line of a tab-separated file: no tabs or line breaks inside a field
std::string Field(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

std::string Num(double v, int decimals = 1) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", decimals, v);
    return b;
}

Machine Read() {
    Machine m;
#ifdef _WIN32
    m.cpu = Trim(RegString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
    int regs[4]{};
    __cpuid(regs, 0);
    if (regs[0] >= 7) {
        __cpuidex(regs, 7, 0);
        m.avx2 = (regs[1] >> 5) & 1;
        m.avx512 = (regs[1] >> 16) & 1;  // AVX-512 Foundation
    }
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<char> buf(len);
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len)) {
        for (DWORD at = 0; at < len;) {
            auto* p = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + at);
            m.cores++;
            at += p->Size;
        }
    }
    m.threads = int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                m.gpu = Trim(Narrow(d.Description));
                m.vram_mb = int64_t(d.DedicatedVideoMemory / (1024 * 1024));
                LARGE_INTEGER v{};
                if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) {
                    char ver[64];
                    std::snprintf(ver, sizeof(ver), "%u.%u.%u.%u", unsigned(HIWORD(v.HighPart)),
                                  unsigned(LOWORD(v.HighPart)), unsigned(HIWORD(v.LowPart)), unsigned(LOWORD(v.LowPart)));
                    m.driver = ver;
                }
                a->Release();
                break;
            }
            a->Release();
        }
        f->Release();
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) m.ram_gb = int((mem.ullTotalPhys + (512ull << 20)) >> 30);
    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    m.windows = std::to_string(RegDword(nt, L"CurrentMajorVersionNumber")) + "." +
                std::to_string(RegDword(nt, L"CurrentMinorVersionNumber")) + "." + RegString(nt, L"CurrentBuild") + "." +
                std::to_string(RegDword(nt, L"UBR"));
#endif
    return m;
}

// the position just after "key": (and its blanks), or npos
size_t ValueAt(const std::string& json, const std::string& key) {
    std::string quoted = "\"" + key + "\"";
    for (size_t at = json.find(quoted); at != std::string::npos; at = json.find(quoted, at + 1)) {
        size_t colon = json.find_first_not_of(" \t\r\n", at + quoted.size());
        if (colon == std::string::npos || json[colon] != ':') continue;
        return json.find_first_not_of(" \t\r\n", colon + 1);
    }
    return std::string::npos;
}

}  // namespace

const Machine& ThisMachine() {
    static const Machine m = Read();
    return m;
}

bool JsonNumber(const std::string& json, const std::string& key, double& out) {
    size_t at = ValueAt(json, key);
    if (at == std::string::npos) return false;
    char* end = nullptr;
    double v = std::strtod(json.c_str() + at, &end);
    if (end == json.c_str() + at) return false;
    out = v;
    return true;
}

namespace {

void AppendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) {
        s += char(c);
    } else if (c < 0x800) {
        s += char(0xC0 | (c >> 6));
        s += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += char(0xE0 | (c >> 12));
        s += char(0x80 | ((c >> 6) & 0x3F));
        s += char(0x80 | (c & 0x3F));
    } else {
        s += char(0xF0 | (c >> 18));
        s += char(0x80 | ((c >> 12) & 0x3F));
        s += char(0x80 | ((c >> 6) & 0x3F));
        s += char(0x80 | (c & 0x3F));
    }
}

uint32_t Hex4(const std::string& s, size_t at) {
    if (at + 4 > s.size()) return 0xFFFD;
    return uint32_t(std::strtoul(s.substr(at, 4).c_str(), nullptr, 16));
}

}  // namespace

std::string JsonString(const std::string& json, const std::string& key) {
    size_t at = ValueAt(json, key);
    if (at == std::string::npos || json[at] != '"') return {};
    std::string s;
    for (size_t i = at + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] != '\\' || i + 1 >= json.size()) {
            s += json[i];
            continue;
        }
        char e = json[++i];
        switch (e) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u': {
                uint32_t c = Hex4(json, i + 1);
                i += 4;
                // a surrogate pair: the second half follows as \uDCxx
                if (c >= 0xD800 && c < 0xDC00 && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u') {
                    uint32_t low = Hex4(json, i + 3);
                    if (low >= 0xDC00 && low < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                AppendUtf8(s, c);
                break;
            }
            default: s += e;  // \" \\ \/
        }
    }
    return s;
}

std::string JsonObject(const std::string& json, const std::string& key) {
    size_t at = ValueAt(json, key);
    if (at == std::string::npos || json[at] != '{') return {};
    int depth = 0;
    bool in_string = false;
    for (size_t i = at; i < json.size(); i++) {
        char c = json[i];
        if (in_string) {
            if (c == '\\') i++;
            else if (c == '"') in_string = false;
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            return json.substr(at, i - at + 1);
        }
    }
    return {};
}

bool ReadGameFile(const fs::path& file, Session& s) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string json = ss.str();
    std::string fps = JsonObject(json, "fps");
    const std::string& src = fps.empty() ? json : fps;
    double v = 0;
    if (JsonNumber(src, fps.empty() ? "fps_medio" : "medio", v)) s.fps_avg = v;
    if (JsonNumber(src, fps.empty() ? "fps_1pct" : "p1", v)) s.fps_p1 = v;
    if (JsonNumber(src, fps.empty() ? "fps_min" : "min", v)) s.fps_min = v;
    if (JsonNumber(src, "amostras", v)) s.samples = int64_t(v);
    if (JsonNumber(json, "minutos", v) && v > 0) s.minutes = v;
    s.build = JsonString(json, "build");
    return s.HasFps();
}

std::string OptionsJson(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string out = "{";
    for (auto& [k, v] : values) {
        if (v.find('\\') != std::string::npos || v.find('/') != std::string::npos || v.find(':') != std::string::npos)
            continue;  // a path, or something that might hold one
        if (out.size() > 1) out += ",";
        char* end = nullptr;
        std::strtod(v.c_str(), &end);
        bool number = !v.empty() && end == v.c_str() + v.size();
        out += Json(k) + ":" + (number || v == "true" || v == "false" ? v : Json(v));
    }
    return out + "}";
}

void AppendHistory(const fs::path& file, const Session& s) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out << s.when << '\t' << Field(s.game) << '\t' << Field(s.port) << '\t' << Num(s.minutes) << '\t' << Num(s.fps_avg)
        << '\t' << Num(s.fps_p1) << '\t' << Num(s.fps_min) << '\t' << s.samples << '\t' << Field(s.options) << '\t'
        << Field(s.build) << '\n';
}

std::vector<Session> LoadHistory(const fs::path& file, const std::string& game) {
    std::vector<Session> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        size_t a = 0;
        for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
        f.push_back(line.substr(a));
        if (f.size() < 10) continue;
        Session s;
        s.when = std::atoll(f[0].c_str());
        s.game = f[1];
        s.port = f[2];
        s.minutes = std::atof(f[3].c_str());
        s.fps_avg = std::atof(f[4].c_str());
        s.fps_p1 = std::atof(f[5].c_str());
        s.fps_min = std::atof(f[6].c_str());
        s.samples = std::atoll(f[7].c_str());
        s.options = f[8];
        s.build = f[9];
        if (game.empty() || s.game == game) out.push_back(s);
    }
    return out;
}

std::string ReportJson(const Machine& m, const Session& s, const std::string& install) {
    std::string o = "{\"versao\":1,\"instalacao\":" + Json(install);
    o += ",\"maquina\":{\"cpu\":" + Json(m.cpu) + ",\"nucleos\":" + std::to_string(m.cores) +
         ",\"threads\":" + std::to_string(m.threads) + ",\"avx2\":" + (m.avx2 ? "true" : "false") +
         ",\"avx512\":" + (m.avx512 ? "true" : "false") + ",\"gpu\":" + Json(m.gpu) +
         ",\"vram_mb\":" + std::to_string(m.vram_mb) + ",\"driver\":" + Json(m.driver) +
         ",\"ram_gb\":" + std::to_string(m.ram_gb) + ",\"windows\":" + Json(m.windows) + "}";
    o += ",\"jogo\":{\"id\":" + Json(s.game) + ",\"port\":" + Json(s.port) + ",\"build\":" + Json(s.build) +
         ",\"resumo\":" + Json(s.summary) + ",\"opcoes\":" + (s.options.empty() ? "{}" : s.options) + "}";
    o += ",\"sessao\":{\"minutos\":" + Num(s.minutes) + ",\"fps_medio\":" + Num(s.fps_avg) +
         ",\"fps_1pct\":" + Num(s.fps_p1) + ",\"fps_min\":" + Num(s.fps_min) +
         ",\"amostras\":" + std::to_string(s.samples) + "}}";
    return o;
}

}  // namespace launcher

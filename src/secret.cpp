#include "secret.h"

#include <cstdint>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace launcher {

namespace {

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Encode(const uint8_t* p, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += kB64[v >> 18 & 63];
        o += kB64[v >> 12 & 63];
        o += i + 1 < n ? kB64[v >> 6 & 63] : '=';
        o += i + 2 < n ? kB64[v & 63] : '=';
    }
    return o;
}

std::vector<uint8_t> Decode(const std::string& s) {
    std::vector<uint8_t> o;
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        const char* at = std::char_traits<char>::find(kB64, 64, c);
        if (!at) continue;  // '=' and anything else
        v = v << 6 | uint32_t(at - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o.push_back(uint8_t(v >> bits));
        }
    }
    return o;
}

#ifdef _WIN32
// a little of the launcher's own in the seal: another program of the same user cannot unseal it blindly
DATA_BLOB Entropy() {
    static char kSalt[] = "otavix-launcher-admin";
    return {DWORD(sizeof(kSalt) - 1), reinterpret_cast<BYTE*>(kSalt)};
}
#endif

}  // namespace

std::string Seal(const std::string& plain) {
#ifdef _WIN32
    if (plain.empty()) return {};
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))}, out{}, salt = Entropy();
    if (!CryptProtectData(&in, L"Otavix Launcher", &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string s = Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
#else
    return {};
#endif
}

std::string Unseal(const std::string& sealed) {
#ifdef _WIN32
    std::vector<uint8_t> bytes = Decode(sealed);
    if (bytes.empty()) return {};
    DATA_BLOB in{DWORD(bytes.size()), bytes.data()}, out{}, salt = Entropy();
    if (!CryptUnprotectData(&in, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string s(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return s;
#else
    (void)sealed;
    return {};
#endif
}

}  // namespace launcher

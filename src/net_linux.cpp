// The launcher's network on Linux / SteamOS: libcurl, loaded when first used (the system's own, so the program does not
// depend on which build of it -- OpenSSL or GnuTLS -- the distribution carries), and the SHA-1 / SHA-256 the downloads
// are checked with. Windows uses WinHTTP and BCrypt in net.cpp.
#ifndef _WIN32

#include "net.h"
#include "i18n.h"

#include <curl/curl.h>
#include <dlfcn.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace launcher {

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------------------------------------ hashes

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf[64];
    uint64_t total = 0;
    size_t fill = 0;
    static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void Block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
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
    void Add(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += n;
        while (n) {
            size_t take = std::min(n, sizeof(buf) - fill);
            std::memcpy(buf + fill, p, take);
            fill += take, p += take, n -= take;
            if (fill == 64) Block(buf), fill = 0;
        }
    }
    std::string Hex() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        Add(&pad, 1);
        pad = 0;
        while (fill != 56) Add(&pad, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        Add(len, 8);
        char out[41];
        for (int i = 0; i < 5; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    uint64_t total = 0;
    size_t fill = 0;
    static uint32_t Ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
    void Block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    void Add(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += n;
        while (n) {
            size_t take = std::min(n, sizeof(buf) - fill);
            std::memcpy(buf + fill, p, take);
            fill += take, p += take, n -= take;
            if (fill == 64) Block(buf), fill = 0;
        }
    }
    std::string Hex() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        Add(&pad, 1);
        pad = 0;
        while (fill != 56) Add(&pad, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        Add(len, 8);
        char out[65];
        for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

// ------------------------------------------------------------------------------------------------ libcurl

struct Curl {
    CURL* (*easy_init)() = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
    CURLcode (*easy_perform)(CURL*) = nullptr;
    CURLcode (*easy_getinfo)(CURL*, CURLINFO, ...) = nullptr;
    const char* (*easy_strerror)(CURLcode) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;
    bool ok = false;
};

const Curl& Lib() {
    static const Curl c = [] {
        Curl l;
        void* so = nullptr;
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"})
            if ((so = dlopen(name, RTLD_NOW))) break;
        if (!so) return l;
        auto sym = [&](const char* n) { return dlsym(so, n); };
        l.easy_init = reinterpret_cast<decltype(l.easy_init)>(sym("curl_easy_init"));
        l.easy_cleanup = reinterpret_cast<decltype(l.easy_cleanup)>(sym("curl_easy_cleanup"));
        l.easy_setopt = reinterpret_cast<decltype(l.easy_setopt)>(sym("curl_easy_setopt"));
        l.easy_perform = reinterpret_cast<decltype(l.easy_perform)>(sym("curl_easy_perform"));
        l.easy_getinfo = reinterpret_cast<decltype(l.easy_getinfo)>(sym("curl_easy_getinfo"));
        l.easy_strerror = reinterpret_cast<decltype(l.easy_strerror)>(sym("curl_easy_strerror"));
        l.slist_append = reinterpret_cast<decltype(l.slist_append)>(sym("curl_slist_append"));
        l.slist_free_all = reinterpret_cast<decltype(l.slist_free_all)>(sym("curl_slist_free_all"));
        auto global = reinterpret_cast<CURLcode (*)(long)>(sym("curl_global_init"));
        if (global) global(CURL_GLOBAL_DEFAULT);
        l.ok = l.easy_init && l.easy_cleanup && l.easy_setopt && l.easy_perform && l.easy_getinfo && l.easy_strerror &&
               l.slist_append && l.slist_free_all;
        return l;
    }();
    return c;
}

size_t Append(char* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(data, size * n);
    return size * n;
}

bool NoLibcurl(std::string& error) {
    error = L3("não encontrei a biblioteca libcurl neste sistema", "libcurl was not found on this system", "no se encontró la biblioteca libcurl en este sistema");
    return false;
}

}  // namespace

HttpResult HttpRequestLinux(const std::string& method, const std::string& url, const Headers& headers, const std::string& body) {
    HttpResult r;
    const Curl& c = Lib();
    if (!c.ok) return NoLibcurl(r.error), r;
    CURL* q = c.easy_init();
    if (!q) return r.error = "libcurl", r;
    curl_slist* hs = nullptr;
    bool typed = false;  // the caller's own Content-Type (a binary upload), else JSON
    for (auto& [k, v] : headers) {
        hs = c.slist_append(hs, (k + ": " + v).c_str());
        if (strcasecmp(k.c_str(), "Content-Type") == 0) typed = true;
    }
    if (!body.empty() && !typed) hs = c.slist_append(hs, "Content-Type: application/json; charset=utf-8");
    c.easy_setopt(q, CURLOPT_URL, url.c_str());
    c.easy_setopt(q, CURLOPT_USERAGENT, "Otavix-Launcher");
    c.easy_setopt(q, CURLOPT_FOLLOWLOCATION, 1L);
    c.easy_setopt(q, CURLOPT_CONNECTTIMEOUT, 8L);
    c.easy_setopt(q, CURLOPT_TIMEOUT, 60L);
    c.easy_setopt(q, CURLOPT_NOSIGNAL, 1L);
    if (hs) c.easy_setopt(q, CURLOPT_HTTPHEADER, hs);
    if (method == "GET") {
        c.easy_setopt(q, CURLOPT_HTTPGET, 1L);
    } else {
        c.easy_setopt(q, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            c.easy_setopt(q, CURLOPT_POSTFIELDS, body.data());
            c.easy_setopt(q, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        }
    }
    c.easy_setopt(q, CURLOPT_WRITEFUNCTION, Append);
    c.easy_setopt(q, CURLOPT_WRITEDATA, &r.body);
    CURLcode rc = c.easy_perform(q);
    if (rc == CURLE_OK) {
        long status = 0;
        c.easy_getinfo(q, CURLINFO_RESPONSE_CODE, &status);
        r.status = int(status);
        if (!r.Ok()) r.error = r.body.empty() ? "servidor respondeu " + std::to_string(status) : r.body;
    } else {
        r.error = L3("sem conexão com o servidor (", "no connection to the server (", "sin conexión con el servidor (") + c.easy_strerror(rc) + ")";
    }
    if (hs) c.slist_free_all(hs);
    c.easy_cleanup(q);
    return r;
}

std::string FileSha1(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Sha1 h;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        if (in.gcount() > 0) h.Add(buf.data(), size_t(in.gcount()));
    }
    return h.Hex();
}

namespace {
struct Down {
    std::ofstream out;
    Sha256 sha;
    std::atomic<bool>* cancel;
    uint64_t* done;
    std::mutex* m;
    bool failed = false;
};

size_t Save(char* data, size_t size, size_t n, void* user) {
    Down* d = static_cast<Down*>(user);
    size_t got = size * n;
    d->out.write(data, std::streamsize(got));
    if (!d->out) return d->failed = true, 0;
    d->sha.Add(data, got);
    std::lock_guard lock(*d->m);
    *d->done += got;
    return got;
}

int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return static_cast<Down*>(user)->cancel->load() ? 1 : 0; }
}  // namespace

// a download into a file, its SHA-256 on the way; redirects (GitHub's assets go to their storage) followed
bool DownloadFile(const std::string& url, const fs::path& file, std::atomic<bool>& cancel, uint64_t& done, std::mutex& m,
                  std::string& sha256, std::string& error) {
    const Curl& c = Lib();
    if (!c.ok) return NoLibcurl(error);
    CURL* q = c.easy_init();
    if (!q) return error = "libcurl", false;
    Down d;
    d.out.open(file, std::ios::binary | std::ios::trunc);
    d.cancel = &cancel, d.done = &done, d.m = &m;
    c.easy_setopt(q, CURLOPT_URL, url.c_str());
    c.easy_setopt(q, CURLOPT_USERAGENT, "Otavix-Launcher");
    c.easy_setopt(q, CURLOPT_FOLLOWLOCATION, 1L);
    c.easy_setopt(q, CURLOPT_CONNECTTIMEOUT, 10L);
    c.easy_setopt(q, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // stalled: under 1 KB/s for 30 s
    c.easy_setopt(q, CURLOPT_LOW_SPEED_TIME, 30L);
    c.easy_setopt(q, CURLOPT_NOSIGNAL, 1L);
    c.easy_setopt(q, CURLOPT_FAILONERROR, 1L);
    c.easy_setopt(q, CURLOPT_WRITEFUNCTION, Save);
    c.easy_setopt(q, CURLOPT_WRITEDATA, &d);
    c.easy_setopt(q, CURLOPT_NOPROGRESS, 0L);
    c.easy_setopt(q, CURLOPT_XFERINFOFUNCTION, Progress);
    c.easy_setopt(q, CURLOPT_XFERINFODATA, &d);
    CURLcode rc = c.easy_perform(q);
    long status = 0;
    c.easy_getinfo(q, CURLINFO_RESPONSE_CODE, &status);
    c.easy_cleanup(q);
    d.out.close();
    if (rc == CURLE_OK) {
        sha256 = d.sha.Hex();
        return true;
    }
    if (cancel) error = "cancelado";
    else if (d.failed) error = "disco cheio?";
    else if (rc == CURLE_HTTP_RETURNED_ERROR) error = L3("o servidor respondeu ", "the server answered ", "el servidor respondió ") + std::to_string(status);
    else error = L3("sem conexão (", "no connection (", "sin conexión (") + c.easy_strerror(rc) + ")";
    return false;
}

}  // namespace launcher

#endif

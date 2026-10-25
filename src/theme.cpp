#include "theme.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "art.h"

namespace launcher {

namespace {

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
int16_t Be16(const uint8_t* p) { return int16_t(uint16_t(p[0] << 8 | p[1])); }

// ------------------------------------------------------------------------------------------------ EA-XA v2
// A frame is 28 samples of one channel: a byte of filter and shift, then 14 bytes of nibbles (high first); 0xEE
// instead starts a frame of two history samples and 28 big-endian PCM samples. History runs on across blocks.

struct History {
    int32_t h1 = 0, h2 = 0;
};

// samples of one channel from data, as many as n; false when data ends first
bool DecodeXa(const uint8_t* data, size_t size, uint32_t n, History& h, std::vector<int16_t>& out) {
    static const int32_t kCoef[20] = {0, 240, 460, 392, 0, 0, -208, -220, 0, 1, 3, 4, 7, 8, 10, 11, 0, -1, -3, -4};
    size_t o = 0;
    for (uint32_t done = 0; done < n;) {
        if (o >= size) return false;
        uint8_t info = data[o];
        uint32_t take = std::min<uint32_t>(28, n - done);
        if (info == 0xEE) {
            if (o + 61 > size) return false;
            h.h1 = Be16(data + o + 1);
            h.h2 = Be16(data + o + 3);
            for (uint32_t i = 0; i < take; i++) out.push_back(Be16(data + o + 5 + i * 2));
            o += 61;
        } else {
            if (o + 15 > size) return false;
            int32_t c1 = kCoef[info >> 4], c2 = kCoef[(info >> 4) + 4];
            int shift = (info & 0x0F) + 8;
            for (uint32_t i = 0; i < take; i++) {
                uint8_t b = data[o + 1 + i / 2];
                int32_t nib = (i & 1) ? (b & 0x0F) : (b >> 4);
                int32_t s = int32_t(uint32_t(nib) << 28) >> shift;
                s = std::clamp((s + c1 * h.h1 + c2 * h.h2) >> 8, -32768, 32767);
                out.push_back(int16_t(s));
                h.h2 = h.h1;
                h.h1 = s;
            }
            o += 15;
        }
        done += take;
    }
    return true;
}

// the PT header's values: channels (0x82), sample rate (0x84), codec (0xA0)
bool ReadPt(const uint8_t* p, size_t size, int& channels, int& rate, int& codec) {
    if (size < 4 || p[0] != 'P' || p[1] != 'T') return false;
    channels = 1;
    rate = 22050;
    codec = -1;
    for (size_t o = 4; o < size;) {
        uint8_t tag = p[o++];
        if (tag == 0xFF) return true;
        if (tag == 0xFC || tag == 0xFD || tag == 0xFE) continue;  // section marks, no value
        if (o >= size) return false;
        uint8_t len = p[o++];
        if (o + len > size || len > 4) return false;
        uint32_t v = 0;
        for (uint8_t i = 0; i < len; i++) v = v << 8 | p[o + i];
        o += len;
        if (tag == 0x82) channels = int(v);
        if (tag == 0x84) rate = int(v);
        if (tag == 0xA0) codec = int(v);
    }
    return false;
}

// one SCHl stream of an EA .mus at an offset, interleaved onto pcm; every stream must share channels and rate
bool DecodeSchl(const std::vector<uint8_t>& mus, size_t at, std::vector<int16_t>& pcm, int& channels, int& rate) {
    if (at + 8 > mus.size() || std::memcmp(&mus[at], "SCHl", 4)) return false;
    uint32_t head = Le32(&mus[at + 4]);
    int ch = 0, sr = 0, codec = 0;
    if (head < 12 || at + head > mus.size() || !ReadPt(&mus[at + 8], head - 8, ch, sr, codec)) return false;
    if (codec != 0x0A || ch < 1 || ch > 2 || (channels && (ch != channels || sr != rate))) return false;  // EA-XA v2 only
    channels = ch;
    rate = sr;
    std::vector<History> hist(static_cast<size_t>(ch));
    std::vector<std::vector<int16_t>> chan(static_cast<size_t>(ch));
    for (size_t o = at + head;;) {
        if (o + 8 > mus.size()) return false;
        uint32_t size = Le32(&mus[o + 4]);
        if (size < 8 || o + size > mus.size()) return false;
        if (!std::memcmp(&mus[o], "SCEl", 4)) break;
        if (!std::memcmp(&mus[o], "SCDl", 4)) {
            if (size < 12 + 4u * ch) return false;
            uint32_t n = Le32(&mus[o + 8]);
            size_t base = o + 12 + 4 * size_t(ch);
            for (int c = 0; c < ch; c++) {
                size_t start = base + Le32(&mus[o + 12 + 4 * size_t(c)]);
                if (start >= o + size) return false;
                if (!DecodeXa(&mus[start], o + size - start, n, hist[size_t(c)], chan[size_t(c)])) return false;
            }
        }
        o += size;
    }
    for (size_t i = 0; i < chan[0].size(); i++)
        for (int c = 0; c < ch; c++) pcm.push_back(chan[size_t(c)][i]);
    return true;
}

// where the first n streams of an EA MPF (version 5, little endian) start in its .mus: its sample table at 0x34 holds
// offsets in 0x80 units
bool MpfOffsets(const std::vector<uint8_t>& mpf, int n, std::vector<size_t>& out) {
    if (mpf.size() < 0x3C || std::memcmp(mpf.data(), "xDFP", 4) || mpf[4] != 5) return false;
    uint32_t table = Le32(&mpf[0x34]), end = Le32(&mpf[0x38]);
    if (end > mpf.size() || table > end || (end - table) / 8 < uint32_t(n)) return false;
    for (int i = 0; i < n; i++) out.push_back(size_t(Le32(&mpf[table + 8 * size_t(i)])) * 0x80);
    return true;
}

bool WriteWav(const fs::path& path, const std::vector<int16_t>& pcm, int channels, int rate) {
    auto le32 = [](std::ofstream& f, uint32_t v) {
        char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
        f.write(b, 4);
    };
    auto le16 = [](std::ofstream& f, uint16_t v) {
        char b[2] = {char(v), char(v >> 8)};
        f.write(b, 2);
    };
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        uint32_t data = uint32_t(pcm.size() * 2);
        f.write("RIFF", 4);
        le32(f, 36 + data);
        f.write("WAVEfmt ", 8);
        le32(f, 16);
        le16(f, 1);
        le16(f, uint16_t(channels));
        le32(f, uint32_t(rate));
        le32(f, uint32_t(rate * channels * 2));
        le16(f, uint16_t(channels * 2));
        le16(f, 16);
        f.write("data", 4);
        le32(f, data);
        f.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(data));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

}  // namespace

ThemeSpec ReadThemeSpec(const fs::path& profile_file) {
    ThemeSpec s;
    toml::table t;
    try {
        t = toml::parse_file(U8(profile_file));
    } catch (...) {
        return s;
    }
    const toml::table* a = t["theme"].as_table();
    if (!a) return s;
    s.kind = (*a)["kind"].value_or(std::string());
    s.file = (*a)["file"].value_or(std::string());
    s.mus = (*a)["mus"].value_or(std::string());
    s.streams = int((*a)["streams"].value_or(int64_t(1)));
    s.fade_in = float((*a)["fade_in"].value_or(0.0));
    s.fade_out = float((*a)["fade_out"].value_or(0.0));
    return s;
}

bool MakeTheme(const ThemeSpec& spec, const fs::path& game, const fs::path& wav) {
    if (spec.kind != "ea-mpf" || spec.streams < 1 || game.empty()) return false;
    std::vector<uint8_t> mpf, mus;
    std::vector<size_t> offsets;
    if (!ReadGameFile(game, spec.file, mpf) || !MpfOffsets(mpf, spec.streams, offsets)) return false;
    // the .mus up to the last stream wanted, with room for it (a stream of a few seconds is about 80 KB)
    if (!ReadGameFile(game, spec.mus, mus, *std::max_element(offsets.begin(), offsets.end()) + 1024 * 1024)) return false;
    std::vector<int16_t> pcm;
    int channels = 0, rate = 0;
    for (size_t at : offsets)
        if (!DecodeSchl(mus, at, pcm, channels, rate)) return false;
    // linear fades in and out
    size_t frames = pcm.size() / size_t(channels);
    size_t in = std::min(frames, size_t(spec.fade_in * float(rate))), out = std::min(frames, size_t(spec.fade_out * float(rate)));
    for (size_t f = 0; f < frames; f++) {
        float g = 1;
        if (f < in) g = float(f) / float(in);
        if (frames - f <= out) g = std::min(g, float(frames - f) / float(out));
        if (g < 1)
            for (int c = 0; c < channels; c++) pcm[f * size_t(channels) + size_t(c)] = int16_t(float(pcm[f * size_t(channels) + size_t(c)]) * g);
    }
    std::error_code ec;
    fs::create_directories(wav.parent_path(), ec);
    return WriteWav(wav, pcm, channels, rate);
}

}  // namespace launcher

#include "music.h"

#include <algorithm>
#include <fstream>
#include <thread>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "stb_vorbis.c"
// stb_vorbis leaves its one-letter macros behind
#undef L
#undef C
#undef R

namespace launcher {

namespace {

constexpr int kRate = 48000;

bool ReadAll(const fs::path& f, std::vector<uint8_t>& out) {
    std::ifstream in(f, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

// any of the three as interleaved stereo floats at 48 kHz
std::vector<float> Decode(const fs::path& file) {
    std::vector<uint8_t> data;
    if (!ReadAll(file, data)) return {};
    std::string ext = U8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<float> src;
    int channels = 0, rate = 0;
    if (ext == ".ogg") {
        short* out = nullptr;
        int frames = stb_vorbis_decode_memory(data.data(), int(data.size()), &channels, &rate, &out);
        if (frames <= 0 || !out) return {};
        src.resize(size_t(frames) * size_t(channels));
        for (size_t i = 0; i < src.size(); i++) src[i] = out[i] / 32768.0f;
        free(out);
    } else if (ext == ".mp3") {
        drmp3_config cfg{};
        drmp3_uint64 frames = 0;
        float* out = drmp3_open_memory_and_read_pcm_frames_f32(data.data(), data.size(), &cfg, &frames, nullptr);
        if (!out) return {};
        channels = int(cfg.channels);
        rate = int(cfg.sampleRate);
        src.assign(out, out + frames * cfg.channels);
        drmp3_free(out, nullptr);
    } else if (ext == ".wav") {
        unsigned int ch = 0, sr = 0;
        drwav_uint64 frames = 0;
        float* out = drwav_open_memory_and_read_pcm_frames_f32(data.data(), data.size(), &ch, &sr, &frames, nullptr);
        if (!out) return {};
        channels = int(ch);
        rate = int(sr);
        src.assign(out, out + frames * ch);
        drwav_free(out, nullptr);
    } else {
        return {};
    }
    if (channels <= 0 || rate <= 0) return {};
    size_t frames = src.size() / size_t(channels);
    // to stereo, then to 48 kHz by linear steps
    auto at = [&](size_t f, int c) { return src[f * size_t(channels) + size_t(std::min(c, channels - 1))]; };
    size_t out_frames = size_t(double(frames) * kRate / rate);
    std::vector<float> out(out_frames * 2);
    double step = double(rate) / kRate;
    for (size_t i = 0; i < out_frames; i++) {
        double p = i * step;
        size_t f = size_t(p);
        float t = float(p - double(f));
        size_t g = std::min(f + 1, frames - 1);
        for (int c = 0; c < 2; c++) out[i * 2 + size_t(c)] = at(f, c) * (1 - t) + at(g, c) * t;
    }
    return out;
}

}  // namespace

fs::path FindTheme(const fs::path& folder) {
    std::error_code ec;
    for (const char* name : {"tema.ogg", "tema.mp3", "tema.wav"})
        if (fs::is_regular_file(folder / name, ec)) return folder / name;
    return {};
}

bool Music::Open() {
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, this);
    if (!stream_) return false;
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void Music::Close() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}

void Music::Play(const fs::path& file) {
    uint64_t token;
    {
        std::lock_guard lock(m_);
        if (file == file_ && target_ > 0) return;
        file_ = file;
        target_ = 0;  // the song playing fades out while the next one decodes
        has_pending_ = false;
        token = ++token_;
    }
    std::thread([this, file, token] {
        std::vector<float> pcm = Decode(file);
        std::lock_guard lock(m_);
        if (token != token_ || pcm.empty()) return;
        pending_ = std::move(pcm);
        has_pending_ = true;
    }).detach();
}

void Music::Stop() {
    std::lock_guard lock(m_);
    file_.clear();
    target_ = 0;
    has_pending_ = false;
    token_++;
}

void Music::SetVolume(float v) {
    std::lock_guard lock(m_);
    volume_ = std::clamp(v, 0.0f, 1.0f);
}

void SDLCALL Music::Feed(void* self, SDL_AudioStream* s, int additional, int) {
    Music& m = *static_cast<Music*>(self);
    int frames = additional / int(sizeof(float) * 2);
    if (frames <= 0) return;
    std::vector<float> out(size_t(frames) * 2, 0.0f);
    {
        std::lock_guard lock(m.m_);
        // the next song takes over once the last one has faded away
        if (m.has_pending_ && m.gain_ <= 0.0001f) {
            m.pcm_ = std::move(m.pending_);
            m.has_pending_ = false;
            m.pos_ = 0;
            m.target_ = 1;
        }
        // in over a second and a half, out over 0.8 s
        const float up = 1.0f / (1.5f * kRate), down = 1.0f / (0.8f * kRate);
        size_t n = m.pcm_.size() / 2;
        for (int i = 0; i < frames; i++) {
            if (m.gain_ < m.target_) m.gain_ = std::min(m.target_, m.gain_ + up);
            else if (m.gain_ > m.target_) m.gain_ = std::max(m.target_, m.gain_ - down);
            if (!n || m.gain_ <= 0) continue;
            float g = m.gain_ * m.gain_ * m.volume_;
            out[size_t(i) * 2] = m.pcm_[m.pos_ * 2] * g;
            out[size_t(i) * 2 + 1] = m.pcm_[m.pos_ * 2 + 1] * g;
            m.pos_ = (m.pos_ + 1) % n;
        }
    }
    SDL_PutAudioStreamData(s, out.data(), int(out.size() * sizeof(float)));
}

}  // namespace launcher

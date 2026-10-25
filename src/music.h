// A game page's theme: <hub>/arte/<id>/tema.ogg (or .mp3, .wav), looping, faded in when the page opens and out when
// it closes or the game starts.
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>

#include "profile.h"

namespace launcher {

// the theme file in a folder, if there is one
fs::path FindTheme(const fs::path& folder);

class Music {
public:
    bool Open();  // the audio device; false leaves the launcher silent
    void Close();
    // decoded in the background, then faded in; the same file already playing goes on
    void Play(const fs::path& file);
    void Stop();
    void SetVolume(float v);  // 0..1

private:
    static void SDLCALL Feed(void* self, SDL_AudioStream* s, int additional, int total);
    std::mutex m_;
    SDL_AudioStream* stream_ = nullptr;
    std::vector<float> pcm_, pending_;  // stereo, 48 kHz
    bool has_pending_ = false;
    size_t pos_ = 0;
    float gain_ = 0, target_ = 0, volume_ = 0.2f;
    fs::path file_;
    uint64_t token_ = 0;
};

}  // namespace launcher

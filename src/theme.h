// A game page's theme made from the player's own copy of the game, for a port whose launcher art has none: the
// profile's [theme] says which audio of the game and how much of it. Nothing of the game ships with the launcher.
#pragma once
#include <string>

#include "profile.h"

namespace launcher {

struct ThemeSpec {
    std::string kind;    // "ea-mpf": EA interactive music, its SCHl streams of EA-XA v2 ADPCM played in the map's order
    std::string file;    // the map (.mpf), relative to the game's folder or disc
    std::string mus;     // ea-mpf: the streams' file (.mus)
    int streams = 1;     // ea-mpf: how many of the map's first streams
    float fade_in = 0;   // seconds
    float fade_out = 0;  // seconds
    bool Empty() const { return kind.empty(); }
};

ThemeSpec ReadThemeSpec(const fs::path& profile_file);
// decodes the theme from the game (its folder or disc image) into a 16-bit WAV file
bool MakeTheme(const ThemeSpec& spec, const fs::path& game, const fs::path& wav);

}  // namespace launcher

// The game's own artwork for the launcher's background, read from the player's game files (a folder or an Xbox disc
// image): nothing of the game ships with the launcher.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

struct ArtImage {
    std::string name;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// the profile's [art]: what to decode, from which file of the game
struct ArtSpec {
    std::string kind;                  // "ea-viv": EA BIGF archive of BIG files of SHPX images; "image": PNG or JPG files
    std::string file;                  // ea-viv: the archive, relative to the game's folder
    std::vector<std::string> include;  // ea-viv: entries, without .big, in showing order; image: the files
    float seconds = 10;                // each picture's time on screen
    bool trim = true;                  // dark borders cut off
    fs::path folder;                   // the pictures' own folder, instead of the game's (a port still being made)
    int card_only = 0;                 // the first pictures are the library card's, not the page's background
    std::vector<std::string> web;      // public pictures by address (a store's box and background), downloaded once
    bool Empty() const { return kind.empty() && web.empty(); }
};

ArtSpec ReadArtSpec(const fs::path& profile_file);
// game: the folder holding the game's files, or its disc image
std::vector<ArtImage> LoadArt(const ArtSpec& spec, const fs::path& game);

// a PNG or JPG file
bool ReadImageFile(const fs::path& file, ArtImage& out);
// the spec's web pictures: downloaded into cache the first time, then read from there
std::vector<ArtImage> LoadWebArt(const ArtSpec& spec, const fs::path& cache);

// a file of the game: from the folder, or from the disc image's XDVDFS; limit: only its first bytes
bool ReadGameFile(const fs::path& game, const std::string& rel, std::vector<uint8_t>& out, size_t limit = SIZE_MAX);
// a PNG or JPG in memory
bool DecodeImage(const std::vector<uint8_t>& bytes, ArtImage& out);
// an Xbox xbe's title image (its $$XTIMAGE section: one DXT texture, the dashboard's icon of the game)
bool XbeTitleImage(const std::vector<uint8_t>& xbe, ArtImage& out);

}  // namespace launcher

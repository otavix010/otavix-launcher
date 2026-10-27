// The consoles' game catalogs (catalogo/<console>.tsv beside the launcher, built by scripts/catalogo.py) and their box
// art, fetched once and kept in cache/capas.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

struct CatalogGame {
    std::string name;   // as shown
    std::string cover;  // libretro box art name, "gametdb:<region>/<id>", or empty
    std::string key;    // for searching
};

// a name for matching: lower case, no accents or punctuation, roman numerals as digits
std::string SearchKey(const std::string& s);

class Catalogs {
public:
    void SetFolder(const fs::path& f) { folder_ = f; }
    // empty when the console has no list
    const std::vector<CatalogGame>& Of(const std::string& console);
    // the best matches for what was typed, at most n
    std::vector<const CatalogGame*> Search(const std::string& console, const std::string& typed, size_t n);

private:
    fs::path folder_;
    std::map<std::string, std::vector<CatalogGame>> lists_;
};

// box art by console and cover reference: a texture once fetched and decoded, else nullptr (and the fetch starts);
// gray, the same box art in shades of grey. A fetch that fails on the way is tried again a little later, three times
class Covers {
public:
    void Open(SDL_Renderer* ren, const fs::path& cache);
    SDL_Texture* Get(const std::string& console, const std::string& cover, float* aspect = nullptr, bool gray = false);
    // no source has that box art (or it never came in three tries): the caller may try another reference
    bool Failed(const std::string& console, const std::string& cover);
    void Take();  // decoded pictures into textures; on the main thread, once a frame
    void Close();

private:
    struct Decoded {
        std::string id;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        int gw = 0, gh = 0;
        std::vector<uint8_t> gray;  // at half the size past 320 wide: it is only shown small
    };
    struct Textures {
        SDL_Texture* color = nullptr;
        SDL_Texture* gray = nullptr;
        float aspect = 0.72f;
    };
    void Worker();
    SDL_Renderer* ren_ = nullptr;
    fs::path cache_;
    std::mutex m_;
    struct Job {
        std::string id;
        std::vector<std::string> urls;  // in turn, until one answers
    };
    std::vector<Job> queue_;
    std::vector<Decoded> done_;
    std::set<std::string> asked_, failed_;
    std::map<std::string, std::pair<int, uint64_t>> retry_;  // a fetch that failed on the way: tries, when to try again
    std::map<std::string, Textures> tex_;
    int workers_ = 0;
    bool closing_ = false;
};

}  // namespace launcher

// The game's icon: its window's (title bar, taskbar, Alt+Tab) while it runs from the launcher, and its desktop
// shortcut's. From the player's game when the profile says where, else a public picture by address (a store's tile),
// downloaded once: as with the art, nothing of the game ships with the launcher.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "art.h"
#include "profile.h"

namespace launcher {

// the profile's [icon]: where the picture comes from, the first that gives one
struct IconSpec {
    std::string kind = "image";      // "image": PNG or JPG; "xbe": an Xbox xbe's title image; "jar": a J2ME jar's MIDlet-Icon
    std::vector<std::string> files;  // in the game (its folder or disc image), or absolute once {option}s are expanded
    std::vector<std::string> web;    // public pictures by address (a store's tile)
    bool pixel = false;              // pixel art: scaled by whole steps, without blur
    bool Empty() const { return files.empty() && web.empty() && kind != "jar"; }
};

IconSpec ReadIconSpec(const fs::path& profile_file);

class GameIcon {
public:
    // in the background: game is the player's game (a folder, a disc image or the jar), empty when it is not there yet;
    // files are the spec's with their {option}s expanded; cache keeps the downloaded picture; ico is the .ico to make
    void Load(const IconSpec& spec, const fs::path& game, const std::vector<std::string>& files, const fs::path& cache,
              const fs::path& ico);
    // the .ico for the shortcut, once made; empty before
    fs::path Ico();
    // while the game runs: its windows that have no icon of their own get this one, each once
    void Apply(uint64_t pid);
    // the game closed: the icons its windows held are freed
    void Release();

private:
    void* Handle(int size);

    std::mutex m_;
    std::atomic<int> loads_ = 0;  // the newest load is the one kept
    ArtImage square_;
    bool pixel_ = false;
    fs::path ico_;
    std::set<void*> seen_;          // windows already given the icon, or with their own
    std::map<int, void*> handles_;  // size -> HICON
    uint64_t next_look_ = 0;        // ms
};

}  // namespace launcher

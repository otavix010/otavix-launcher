// What a report says about the player's computer, and the end of the game's last log.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "profile.h"

namespace launcher {

// key (os, cpu, gpu, driver, ram, display), value
std::vector<std::pair<std::string, std::string>> SystemInfo();

// the newest file in a folder; empty when none
fs::path NewestFile(const fs::path& folder);
// a text file's last bytes, from a line's start
std::string FileTail(const fs::path& file, size_t max_bytes);
// the start and the end of a file, at most head + tail bytes, whole lines, a note where the middle was left out (a
// game's log: the start says the card, the monitor and how frames reach it; the end, how it stopped)
std::string FileHeadTail(const fs::path& file, size_t head, size_t tail);

}  // namespace launcher

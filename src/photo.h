// Pictures for a problem report: a file the player picks or the picture on the clipboard (Win+Shift+S), made a JPEG
// no larger than 1920 px on its longest side. A full-size PNG screenshot is 2-4 MB; the server keeps each attachment
// under 2 MB and gets each picture in a request of its own. Windows only (the system's WIC codecs); elsewhere a JPEG
// or PNG that already fits goes as it is.
#pragma once
#include <filesystem>
#include <string>

namespace launcher {

namespace fs = std::filesystem;

struct Photo {
    std::string name;   // as the player sees it
    std::string bytes;  // JPEG (or the file's own JPEG/PNG where it cannot be converted)
    int width = 0, height = 0;
};

bool PhotoFromFile(const fs::path& file, Photo& out, std::string& error);
// false with an empty error: no picture on the clipboard
bool PhotoFromClipboard(Photo& out, std::string& error);

}  // namespace launcher

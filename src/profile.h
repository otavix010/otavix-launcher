// The game's launcher.toml: what the launcher shows and where each choice goes.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

namespace launcher {

namespace fs = std::filesystem;

// paths and UTF-8 strings: the launcher's text is UTF-8 everywhere
inline std::string U8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
inline fs::path FromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

enum class Lang { Pt, En, Es };

// a text in each language the launcher speaks; a plain string in the profile serves all three
struct Text {
    std::string pt, en, es;
    const std::string& Get(Lang l) const;
    bool Empty() const { return pt.empty() && en.empty() && es.empty(); }
};

using Value = std::variant<bool, int64_t, double, std::string>;

enum class OptType {
    Choice,      // one of choices
    Toggle,      // on or off
    Slider,      // whole number from min to max
    Resolution,  // width and height keys from the monitor's modes, 0 x 0 the desktop's
    ButtonMap,   // a table of console buttons, each the pad button that gives it
    Path,        // a folder or file the player picks: the game's files
};

// where a choice is kept: the game's settings file, an environment variable of the game's process, or the
// launcher's own state (launcher_state.toml)
// Settings: the game's settings file ("section.key" for a key in a section); Env: a variable the game starts with;
// Launcher: the launcher's own state; Ini: "section.key" in the game's .ini file ([game] ini)
enum class Target { Settings, Env, Launcher, Ini };

struct ChoiceItem {
    Value value;
    Text label;
    std::vector<std::pair<std::string, Value>> set;  // instead of value: several settings keys at once
};

struct ButtonSlot {
    std::string key;  // in the settings table
    Text label;       // the console's name for it
    std::string def;  // SDL gamepad name: a, b, x, y, leftshoulder, lefttrigger, dpup, start, ...
};

struct Option {
    std::string page, key;
    OptType type = OptType::Choice;
    Target target = Target::Settings;
    Text label, help;
    Value def = int64_t(0);
    std::vector<ChoiceItem> choices;
    int64_t min = 0, max = 100, step = 1;
    std::string suffix;
    std::string width_key = "width", height_key = "height";  // Resolution
    int64_t guest_height = 0;  // Slider of render percent: shows the internal height for this console height
    std::vector<ButtonSlot> buttons;  // ButtonMap
    std::string pick = "both";        // Path: folder, file or both
    std::vector<std::string> filters; // Path: file extensions without the dot
};

struct Page {
    std::string id;
    Text label;
    bool advanced = false;  // a tab of the advanced options page rather than a page of its own
};

struct Preset {
    Text label, help;
    std::vector<std::pair<std::string, Value>> values;  // option key, value
};

// [install] kind = "build": a folder or file the player points the generator at
struct BuildInput {
    std::string key;                  // its {key} in the command, kept in the port's launcher state
    Text label, help;
    bool folder = true, file = false;  // what may be picked
    std::vector<std::string> filters;  // a file's extensions
    // an optional input: what the generator does by itself when the player picks nothing (then {key} and the flag
    // before it are left out of the command); empty for a needed input
    Text auto_text;
    std::vector<std::pair<Text, std::string>> links;  // official pages, opened in the browser: label, address
    // a file an official server hands out free (a game's update on the console maker's server): downloaded into the
    // port's dl folder, its SHA-1 checked, and then the input's value
    std::string download_url, download_sha1, download_file;
    Text download_label;
    uint64_t download_size = 0;
    // a file that marks the input's folder (PS3_GAME/PARAM.SFO): an archive or a disc image picked instead, or found in
    // the folder picked, is unpacked into the port's folder, and the folder holding the mark becomes the input
    std::string unpack;
};
// a step of the generator: <id>.ok in the marks folder once done, <id>.log while it runs
struct BuildStep {
    std::string id;
    Text label;
};
// a tool the generator needs: there when its check exits 0 (and prints something, with output), else installed by
// a command the player watches in a console
struct Prereq {
    Text name, help;
    std::string check, install;
    bool output = false;
};

// [install]: how a port's game is checked when the player installs it
struct InstallSpec {
    std::string check;             // a file of the game (in its disc image or folder) whose SHA-1 names the release
    std::string sha1;              // the release the port was made from
    Text version;                  // that release, as the player would look for it
    std::string folder = "jogo";   // where an archive's game is kept, in the port's folder
    bool unpack_images = false;    // a disc image is unpacked too (a generator's input wants the files, not the image)
    std::vector<std::string> files;  // a game that is one file: its extensions (jar); copied into folder, its SHA-1 checked
    Text help;                       // what the player picks, for such a game
    std::string kind;                // "build": the game is made on the player's PC by the port's generator
    std::string done;                // build: the file there once the game is made
    std::string marks;               // build: the folder of the steps' marks and logs
    std::vector<std::string> command, redo;  // build: the generator ({dir}, {key}); redo: added to make it all again
    std::string needs;               // build: a file the made game reads from the player's copy, relative to the port
                                     // ("vfs/PS3_GAME/PARAM.SFO", through the generator's link) or an input ("{iso}");
                                     // missing, the copy was moved or deleted and is picked again
    std::vector<BuildInput> inputs;
    std::vector<BuildStep> steps;
    std::vector<Prereq> prereqs;
    Text time;                       // build: how long it takes
};

struct Profile {
    Text title, subtitle, legal;
    Text perf_note;             // [performance] note: what the game's performance page says first (what weighs most,
                                // where to start)
    std::string status = "ok";  // ok, or wip: listed in its library, not playable yet
    Text status_text;           // what the card says when it is not playable
    std::string mods_folder;    // [mods] folder: the port reads mods from there; empty, no mods
    std::string mods_kind;      // [mods] kind: "psxrecomp" for PSXRecomp's packages and state.toml; else zip mods
    std::string mods_game_id;   // [mods] game_id: the disc's id, for the packages meant for it (PSXRecomp)
    Text mods_help;             // [mods] help: how this port's mods are laid out
    InstallSpec install;
    std::string exe;                  // beside the launcher
    std::vector<std::string> exes;    // exe = [...]: the first that exists (one build per CPU)
    bool exe_workdir = false;         // workdir = "exe": run in the executable's folder, not the port's
    std::vector<std::pair<std::string, std::string>> env;  // [game] env: variables for the game, {dir} replaced
    std::string log_file;             // the game's output is written there
    std::string port_file;            // the file that tells the port's own files are there (default: exe)
    std::string settings = "settings.toml";
    std::string ini;  // [game] ini: an .ini file of the game's, beside the settings, for options with target = "ini"
    std::string ini_template;  // [game] ini_template: what that .ini starts as when the game has not made it yet
    std::string logs = "logs";
    std::vector<std::string> saves;   // folders beside the exe holding the saves
    std::string game_file;            // the file a game folder must hold (default.xbe, GOWA.elf ...)
    std::string game_option;          // the Path option holding where the game is
    std::vector<std::string> search;  // folders beside the exe or above it searched when that option is empty
    std::string exe_option;           // an option whose value is the executable, for builds per CPU
    std::vector<std::string> args;    // arguments, {key} replaced by that option's value
    std::array<float, 3> accent{0.9f, 0.25f, 0.1f};
    std::vector<Page> pages;
    std::vector<Option> options;
    std::vector<Preset> presets;
    std::vector<std::pair<Text, Text>> credits;  // name, what for
    std::vector<std::pair<std::string, Value>> defaults;  // what the game takes for a settings key it lacks

    const Value* DefaultFor(const std::string& key) const;

    bool Load(const fs::path& file, std::string& error);
    const Option* Find(const std::string& key) const;
};

}  // namespace launcher

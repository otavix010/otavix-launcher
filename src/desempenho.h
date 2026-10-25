// Performance: what the player's computer is, how each game ran on it, and, only if the player agrees, the same sent to
// the launcher's server, so that everyone can see how the games run on different machines.
//
// A game tells its frame rate through a file: the launcher names it in OTAVIX_DESEMPENHO (and GOWA_PERF_JSON) when it
// starts the game, the game writes it now and then and as it closes, and the launcher reads it when the game is gone.
// Its fields, all optional: {"versao":1, "build":"avx512", "fps":{"medio":59.8,"p1":52,"min":41,"amostras":1800},
// "minutos":30.2, "opcoes":{...}}; the flat names fps_medio, fps_1pct, fps_min and amostras are read too.
//
// What is kept and sent has no name, e-mail, Windows user, file path nor address: the computer's parts by model, the
// Windows build, the game and its port's version, the picture options chosen, the frame rate and the minutes played,
// and a random number made for this install of the launcher.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "profile.h"

namespace launcher {

struct Machine {
    std::string cpu;
    int cores = 0, threads = 0;
    bool avx2 = false, avx512 = false;
    std::string gpu;
    int64_t vram_mb = 0;
    std::string driver;
    int ram_gb = 0;
    std::string windows;  // the build, 10.0.26100.1234
};

// read once, then kept
const Machine& ThisMachine();

struct Session {
    int64_t when = 0;          // its end, Unix seconds
    std::string game, port;    // the game's id and its port's version
    double minutes = 0;
    double fps_avg = 0, fps_p1 = 0, fps_min = 0;
    int64_t samples = 0;       // seconds measured: 0 when the game wrote no frame rate
    std::string options;       // the picture options, a JSON object on one line
    std::string summary;       // the options that matter most to the frame rate, "fps=60;resolution_scale=2":
                               // the server groups by it
    std::string build;         // the game's own build kind ("avx512", "nativo"...), when it says
    bool HasFps() const { return samples > 0 && fps_avg > 0; }
};

// the game's file, read after it closed; false when it wrote none (or nothing it measured)
bool ReadGameFile(const fs::path& file, Session& s);

// the options worth comparing between machines, as a JSON object: those of the profile that are a choice, a toggle,
// a slider or a resolution, never a path or a button map, and no text that looks like a path
std::string OptionsJson(const std::vector<std::pair<std::string, std::string>>& values);

// the local history, a line per session (tab-separated), oldest first
void AppendHistory(const fs::path& file, const Session& s);
std::vector<Session> LoadHistory(const fs::path& file, const std::string& game = "");

// exactly what the server gets for a session
std::string ReportJson(const Machine& m, const Session& s, const std::string& install);

// a JSON object's field, looked for at any depth by its name (the first found): a number, or a string's text
bool JsonNumber(const std::string& json, const std::string& key, double& out);
std::string JsonString(const std::string& json, const std::string& key);
// an object's text, braces included
std::string JsonObject(const std::string& json, const std::string& key);

}  // namespace launcher

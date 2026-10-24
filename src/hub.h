// The hub: hub.toml beside the launcher lists the libraries and each port's folder; hub_state.toml keeps the
// launcher's own settings (language, window, updates).
#pragma once
#include <array>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

struct Library {
    std::string id;
    Text name;
    std::array<float, 3> accent{0.5f, 0.5f, 0.5f};
};

// a port's state on a system: ok (playable), wip (in development), soon (planned); missing, not listed there
enum class Platform { Windows, Linux };

struct GameEntry {
    std::string id;
    std::string library;
    std::string on_windows, on_linux;  // its state on each system
    fs::path dir;      // the port's folder: its executable, settings and saves
    fs::path profile;  // its launcher.toml
    std::string repo;  // owner/name on GitHub: the port's files come from its releases

    const std::string& On(Platform p) const { return p == Platform::Windows ? on_windows : on_linux; }
};

struct HubConfig {
    std::string name = "Otavix Launcher";
    std::string version = "0.0.0";
    std::string update_repo;  // owner/repo on GitHub, empty until the project is published
    std::string server_url;   // reports and port requests; empty until the server is up
    std::array<float, 3> accent{0.86f, 0.12f, 0.15f};
    std::vector<Library> libraries;
    std::vector<GameEntry> games;

    // hub_dir: the launcher's folder; relative paths in hub.toml start there
    bool Load(const fs::path& hub_dir, std::string& error);
};

}  // namespace launcher

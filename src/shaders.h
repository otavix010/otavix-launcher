// The community's shader packs. A port that compiles shaders names its cache in [shaders]; the newest pack of its
// repository (the release "shaders": shaders.zip and shaders.zip.sha256) is merged into that cache before the game
// opens, and, when the player agreed to share, what the game compiled that was neither sent nor in the pack goes to
// the launcher's server once the game closes. Packs are made and checked on the owner's PC before anyone gets them.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "net.h"
#include "profile.h"

namespace launcher {

struct ShaderSpec {
    // "gowa": one file per shader (the PS3 ports); "xenia": the ReXGlue ports' shareable storage files, <TITLEID>.xsh
    // with the guest shaders and <TITLEID>.<rtv|rov>.d3d12.xpso with the pipelines
    std::string kind;
    std::string folder;              // the cache, relative to the port's folder
    std::vector<std::string> files;  // which of its files: patterns such as "*.bin"
    bool Empty() const { return kind.empty() || folder.empty(); }
};

ShaderSpec ReadShaderSpec(const fs::path& profile_file);

class ShaderShare {
public:
    struct Status {
        bool busy = false;
        int pack_count = -1;   // shaders (or storage records) the installed pack brought; -1 none yet
        std::string pack_day;  // when it was installed, dd/mm/yyyy
        int sent = 0;          // files this computer has sent for this game
    };
    // the installed pack and what was sent, from the game's data folder (once, for its page)
    void Load(const fs::path& data_dir);
    // in the background: the repository's newest pack merged into the cache. The game must be closed: ReXGlue keeps
    // its storage files open while it runs
    void Apply(const std::string& repo, const ShaderSpec& spec, const fs::path& port_dir, const fs::path& data_dir);
    // in the background, after the game closed: the cache's new files to the server
    void Send(const std::string& server, const Headers& headers, const std::string& game, const ShaderSpec& spec,
              const fs::path& port_dir, const fs::path& data_dir);
    void Wait();  // until the work in the background is done
    Status Get();

private:
    std::mutex m_;
    Status st_;
};

}  // namespace launcher

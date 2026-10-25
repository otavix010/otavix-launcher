// The mods of a port made with PSXRecomp (Silent Hill): its packages under <mods>/bundled (shipped with the build) and
// <mods>/installed (added by the player), each <package>/<version>/manifest.toml, and the player's choices in
// <mods>/state.toml, which the game reads when it starts. A package with no entry there keeps its manifest's
// defaults; entries of packages that are gone stay as they were (the game leaves them dormant).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

struct PsxChoice {
    std::string value, label;
};

struct PsxOption {
    std::string feature, id, label, description, type;  // type: boolean, choice, integer (else shown as text)
    std::string def;                                     // the manifest's default, as text
    std::vector<PsxChoice> choices;
    int64_t min = 0, max = 100, step = 1;
};

struct PsxFeature {
    std::string id, name, description, group;
    bool default_enabled = false;
};

struct PsxPackage {
    std::string id, version, name, author, description;
    bool installed = false;  // from mods/installed (the player's), not shipped with the build
    std::vector<PsxFeature> features;
    std::vector<PsxOption> options;
    bool OfTheGame() const { return id.rfind("psx.", 0) != 0; }  // the framework's own start with "psx."
};

class PsxMods {
public:
    // the packages meant for this game (its game_id or every game, "*"), the newest version of each
    void Load(const fs::path& mods, const std::string& game_id);
    bool Loaded() const { return loaded_; }
    const std::vector<PsxPackage>& Packages() const { return packages_; }
    const fs::path& Folder() const { return dir_; }

    bool Enabled(const PsxPackage& p, const PsxFeature& f) const;
    void SetEnabled(const PsxPackage& p, const PsxFeature& f, bool on);
    std::string Get(const PsxPackage& p, const PsxOption& o) const;  // the player's, else the default
    void Set(const PsxPackage& p, const PsxOption& o, const std::string& v);
    bool Save();  // state.toml; false when it could not be written
    const std::string& Error() const { return error_; }

private:
    struct FeatureState {
        std::string package, feature;
        bool has_enabled = false, enabled = false;
        std::vector<std::pair<std::string, std::string>> values;  // option id, value
    };
    FeatureState& State(const std::string& package, const std::string& feature);
    const FeatureState* Find(const std::string& package, const std::string& feature) const;

    fs::path dir_;
    std::vector<PsxPackage> packages_;
    std::vector<std::pair<std::string, std::string>> chosen_versions_;  // [[package]] of state.toml: id, version
    std::vector<FeatureState> state_;
    bool loaded_ = false;
    std::string error_;
};

}  // namespace launcher

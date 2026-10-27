// Where the choices live: the game's settings file, and the launcher's own state beside it.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

// a plain .ini kept line by line: the player's comments, order and unknown keys stay as they were
class IniFile {
public:
    void Load(const fs::path& f);
    void LoadText(const std::string& text);
    std::optional<std::string> Get(const std::string& section, const std::string& key) const;
    void Set(const std::string& section, const std::string& key, const std::string& value);
    std::string Text() const;

private:
    std::vector<std::string> lines_;
};

class Store {
public:
    // ini: the game's .ini for options with target = "ini" (empty when the profile has none); ini_template, what it
    // starts as while the game has not made it
    void Open(const fs::path& game_settings, const fs::path& own_state, const fs::path& ini = {},
              const std::string& ini_template = {});
    bool Save();  // both files; false when one could not be written
    bool Dirty() const { return dirty_; }

    // an option's value: as stored, else its default
    Value Get(const Option& o) const;
    void Set(const Option& o, const Value& v);
    // an entry of a ButtonMap option's table
    std::string GetButton(const Option& o, const ButtonSlot& b) const;
    void SetButton(const Option& o, const ButtonSlot& b, const std::string& name);
    // a settings key read as def's type, def when missing; written as is
    Value GetKey(const std::string& key, const Value& def) const;
    void SetKey(const std::string& key, const Value& v);
    // plain keys of the game's settings file (Resolution writes two)
    int64_t GetInt(const std::string& key, int64_t def) const;
    void SetInt(const std::string& key, int64_t v);
    std::string GetString(const std::string& key) const;
    void SetString(const std::string& key, const std::string& v);
    // the launcher's own state
    std::string GetOwn(const std::string& key, const std::string& def = {}) const;
    void SetOwn(const std::string& key, const std::string& v);
    bool GetOwnBool(const std::string& key, bool def) const;
    void SetOwnBool(const std::string& key, bool v);
    // before the game starts: the default of each option kept in the game's own files (settings, .ini) and of each
    // [defaults] key that is not there yet, so that what the launcher shows is what the game uses. Left out, the
    // port's own default ruled: VRR showed off in the launcher and the 360 games and FIFA ran with it on (27/10)
    void FillDefaults(const Profile& p);
    // the environment the game starts with: options whose target is env
    std::map<std::string, std::string> Environment(const Profile& p) const;

private:
    toml::table& TableFor(Target t);
    const toml::table& TableFor(Target t) const;

    fs::path game_file_, own_file_, ini_file_;
    toml::table game_, own_;
    IniFile ini_;
    bool dirty_ = false, ini_dirty_ = false;
};

std::string ToText(const Value& v);
bool SameValue(const Value& a, const Value& b);

}  // namespace launcher

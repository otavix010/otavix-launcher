#include "store.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace launcher {

namespace {

toml::table ParseOrEmpty(const fs::path& f) {
    std::error_code ec;
    if (!fs::exists(f, ec)) return {};
    try {
        return toml::parse_file(U8(f));
    } catch (...) {
        return {};
    }
}

bool Write(const fs::path& f, const toml::table& t) {
    // written beside and renamed over: a crash halfway never leaves half a file
    fs::path tmp = f;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << t << "\n";
        if (!o) return false;
    }
    std::error_code ec;
    fs::rename(tmp, f, ec);
    return !ec;
}

bool Read(const toml::node* n, const Value& like, Value& out) {
    if (!n) return false;
    if (std::holds_alternative<bool>(like)) {
        if (auto b = n->value<bool>()) out = *b;
        else return false;
    } else if (std::holds_alternative<int64_t>(like)) {
        if (auto i = n->value<int64_t>()) out = *i;
        else return false;
    } else if (std::holds_alternative<double>(like)) {
        if (auto d = n->value<double>()) out = *d;
        else return false;
    } else {
        if (auto s = n->value<std::string>()) out = *s;
        else return false;
    }
    return true;
}

void Put(toml::table& t, const std::string& key, const Value& v) {
    std::visit([&](const auto& x) { t.insert_or_assign(key, x); }, v);
}

// "video.fullscreen": the value in the table video, made when missing
void PutPath(toml::table& t, const std::string& path, const Value& v) {
    toml::table* cur = &t;
    size_t at = 0;
    for (size_t dot; (dot = path.find('.', at)) != std::string::npos; at = dot + 1) {
        std::string part = path.substr(at, dot - at);
        if (!cur->contains(part) || !(*cur)[part].is_table()) cur->insert_or_assign(part, toml::table{});
        cur = (*cur)[part].as_table();
    }
    Put(*cur, path.substr(at), v);
}

bool WriteText(const fs::path& f, const std::string& text) {
    fs::path tmp = f;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << text;
        if (!o) return false;
    }
    std::error_code ec;
    fs::rename(tmp, f, ec);
    return !ec;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "section.key" split at the first dot
void SectionKey(const std::string& path, std::string& section, std::string& key) {
    size_t dot = path.find('.');
    section = dot == std::string::npos ? std::string() : path.substr(0, dot);
    key = dot == std::string::npos ? path : path.substr(dot + 1);
}

// an .ini value as the option's kind
bool FromText(const std::string& s, const Value& like, Value& out) {
    try {
        if (std::holds_alternative<bool>(like)) {
            std::string l = LowerAscii(s);
            out = l == "1" || l == "true" || l == "yes" || l == "on";
        } else if (std::holds_alternative<int64_t>(like)) {
            out = int64_t(std::stoll(s));
        } else if (std::holds_alternative<double>(like)) {
            out = std::stod(s);
        } else {
            out = s;
        }
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

std::string ToText(const Value& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, bool>) return x ? "1" : "0";
            else if constexpr (std::is_same_v<T, std::string>) return x;
            else return std::to_string(x);
        },
        v);
}

bool SameValue(const Value& a, const Value& b) {
    if (a.index() == b.index()) return a == b;
    // an integer in the file and a float in the profile, or the other way
    auto num = [](const Value& v, double& d) {
        if (auto i = std::get_if<int64_t>(&v)) return d = double(*i), true;
        if (auto f = std::get_if<double>(&v)) return d = *f, true;
        return false;
    };
    double x, y;
    return num(a, x) && num(b, y) && std::fabs(x - y) < 1e-9;
}

void IniFile::Load(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::ostringstream all;
    all << in.rdbuf();
    LoadText(all.str());
}

void IniFile::LoadText(const std::string& text) {
    lines_.clear();
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(line);
    }
    while (!lines_.empty() && Trim(lines_.back()).empty()) lines_.pop_back();
}

std::optional<std::string> IniFile::Get(const std::string& section, const std::string& key) const {
    std::string cur;
    for (const std::string& raw : lines_) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            cur = LowerAscii(Trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos && cur == LowerAscii(section) && LowerAscii(Trim(line.substr(0, eq))) == LowerAscii(key))
            return Trim(line.substr(eq + 1));
    }
    return std::nullopt;
}

void IniFile::Set(const std::string& section, const std::string& key, const std::string& value) {
    std::string cur, want = LowerAscii(section);
    size_t section_end = std::string::npos;  // after the section's last line with something on it
    bool in_section = want.empty();
    if (in_section) section_end = 0;
    for (size_t i = 0; i < lines_.size(); i++) {
        std::string line = Trim(lines_[i]);
        if (!line.empty() && line[0] == '[') {
            cur = LowerAscii(Trim(line.substr(1, line.find(']') - 1)));
            in_section = cur == want;
            if (in_section) section_end = i + 1;
            continue;
        }
        if (!in_section) continue;
        if (!line.empty()) section_end = i + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos && LowerAscii(Trim(line.substr(0, eq))) == LowerAscii(key)) {
            lines_[i] = key + " = " + value;
            return;
        }
    }
    if (section_end == std::string::npos) {
        if (!lines_.empty() && !Trim(lines_.back()).empty()) lines_.push_back("");
        lines_.push_back("[" + section + "]");
        section_end = lines_.size();
    }
    lines_.insert(lines_.begin() + std::ptrdiff_t(section_end), key + " = " + value);
}

std::string IniFile::Text() const {
    std::ostringstream o;
    for (const std::string& l : lines_) o << l << "\r\n";
    return o.str();
}

void Store::Open(const fs::path& game_settings, const fs::path& own_state, const fs::path& ini,
                 const std::string& ini_template) {
    game_file_ = game_settings;
    own_file_ = own_state;
    ini_file_ = ini;
    game_ = ParseOrEmpty(game_file_);
    own_ = ParseOrEmpty(own_file_);
    std::error_code ec;
    if (!ini_file_.empty()) {
        if (fs::exists(ini_file_, ec)) ini_.Load(ini_file_);
        else ini_.LoadText(ini_template);
    }
    dirty_ = ini_dirty_ = false;
}

bool Store::Save() {
    bool ok = Write(game_file_, game_) && Write(own_file_, own_);
    if (ok && ini_dirty_ && !ini_file_.empty()) {
        ok = WriteText(ini_file_, ini_.Text());
        if (ok) ini_dirty_ = false;
    }
    if (ok) dirty_ = false;
    return ok;
}

toml::table& Store::TableFor(Target t) { return t == Target::Settings ? game_ : own_; }
const toml::table& Store::TableFor(Target t) const { return t == Target::Settings ? game_ : own_; }

Value Store::Get(const Option& o) const {
    if (o.target == Target::Ini) {
        std::string section, key;
        SectionKey(o.key, section, key);
        Value v = o.def;
        if (auto s = ini_.Get(section, key); !s || !FromText(*s, o.def, v)) return o.def;
        return v;
    }
    const toml::table& t = TableFor(o.target);
    std::string key = o.target == Target::Env ? "env." + o.key : o.key;
    const toml::node* n = t.at_path(key).node();
    Value v = o.def;
    if (!Read(n, o.def, v)) return o.def;
    return v;
}

void Store::Set(const Option& o, const Value& v) {
    if (o.target == Target::Ini) {
        std::string section, key;
        SectionKey(o.key, section, key);
        ini_.Set(section, key, ToText(v));
        ini_dirty_ = dirty_ = true;
        return;
    }
    toml::table& t = TableFor(o.target);
    if (o.target == Target::Env) {
        if (!t.contains("env")) t.insert("env", toml::table{});
        Put(*t["env"].as_table(), o.key, v);
    } else {
        PutPath(t, o.key, v);
    }
    dirty_ = true;
}

// a ButtonMap's table: o.key (a table in the settings, or a section of the .ini), one entry per button
std::string Store::GetButton(const Option& o, const ButtonSlot& b) const {
    if (o.target == Target::Ini) return ini_.Get(o.key, b.key).value_or(b.def);
    if (const toml::table* tb = game_.at_path(o.key).as_table())
        if (auto s = (*tb)[b.key].value<std::string>()) return *s;
    return b.def;
}

void Store::SetButton(const Option& o, const ButtonSlot& b, const std::string& name) {
    if (o.target == Target::Ini) {
        ini_.Set(o.key, b.key, name);
        ini_dirty_ = dirty_ = true;
        return;
    }
    PutPath(game_, o.key + "." + b.key, name);
    dirty_ = true;
}

Value Store::GetKey(const std::string& key, const Value& def) const {
    Value v = def;
    if (!Read(game_.at_path(key).node(), def, v)) return def;
    return v;
}

void Store::SetKey(const std::string& key, const Value& v) {
    PutPath(game_, key, v);
    dirty_ = true;
}

int64_t Store::GetInt(const std::string& key, int64_t def) const { return game_[key].value_or(def); }
void Store::SetInt(const std::string& key, int64_t v) {
    game_.insert_or_assign(key, v);
    dirty_ = true;
}
std::string Store::GetString(const std::string& key) const { return game_[key].value_or(std::string()); }
void Store::SetString(const std::string& key, const std::string& v) {
    game_.insert_or_assign(key, v);
    dirty_ = true;
}

std::string Store::GetOwn(const std::string& key, const std::string& def) const { return own_[key].value_or(def); }
void Store::SetOwn(const std::string& key, const std::string& v) {
    own_.insert_or_assign(key, v);
    dirty_ = true;
}
bool Store::GetOwnBool(const std::string& key, bool def) const { return own_[key].value_or(def); }
void Store::SetOwnBool(const std::string& key, bool v) {
    own_.insert_or_assign(key, v);
    dirty_ = true;
}

std::map<std::string, std::string> Store::Environment(const Profile& p) const {
    std::map<std::string, std::string> env;
    // an empty value leaves the variable unset: the game's own default (GOWA_PIN="" read as "on" forced the
    // pinning that its unset default decides by the CPU)
    for (const Option& o : p.options)
        if (o.target == Target::Env)
            if (std::string v = ToText(Get(o)); !v.empty()) env[o.key] = v;
    return env;
}

}  // namespace launcher

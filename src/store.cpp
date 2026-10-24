#include "store.h"

#include <cmath>
#include <fstream>

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

void Store::Open(const fs::path& game_settings, const fs::path& own_state) {
    game_file_ = game_settings;
    own_file_ = own_state;
    game_ = ParseOrEmpty(game_file_);
    own_ = ParseOrEmpty(own_file_);
    dirty_ = false;
}

bool Store::Save() {
    bool ok = Write(game_file_, game_) && Write(own_file_, own_);
    if (ok) dirty_ = false;
    return ok;
}

toml::table& Store::TableFor(Target t) { return t == Target::Settings ? game_ : own_; }
const toml::table& Store::TableFor(Target t) const { return t == Target::Settings ? game_ : own_; }

Value Store::Get(const Option& o) const {
    const toml::table& t = TableFor(o.target);
    std::string key = o.target == Target::Env ? "env." + o.key : o.key;
    const toml::node* n = o.target == Target::Env ? t.at_path(key).node() : t.get(o.key);
    Value v = o.def;
    if (!Read(n, o.def, v)) return o.def;
    return v;
}

void Store::Set(const Option& o, const Value& v) {
    toml::table& t = TableFor(o.target);
    if (o.target == Target::Env) {
        if (!t.contains("env")) t.insert("env", toml::table{});
        Put(*t["env"].as_table(), o.key, v);
    } else {
        Put(t, o.key, v);
    }
    dirty_ = true;
}

std::string Store::GetButton(const Option& o, const ButtonSlot& b) const {
    if (const toml::table* tb = game_[o.key].as_table())
        if (auto s = (*tb)[b.key].value<std::string>()) return *s;
    return b.def;
}

void Store::SetButton(const Option& o, const ButtonSlot& b, const std::string& name) {
    if (!game_.contains(o.key) || !game_[o.key].is_table()) game_.insert_or_assign(o.key, toml::table{});
    game_[o.key].as_table()->insert_or_assign(b.key, name);
    dirty_ = true;
}

Value Store::GetKey(const std::string& key, const Value& def) const {
    Value v = def;
    if (!Read(game_.get(key), def, v)) return def;
    return v;
}

void Store::SetKey(const std::string& key, const Value& v) {
    Put(game_, key, v);
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
    for (const Option& o : p.options)
        if (o.target == Target::Env) env[o.key] = ToText(Get(o));
    return env;
}

}  // namespace launcher

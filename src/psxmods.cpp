#include "psxmods.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace launcher {

namespace {

std::string AsText(const toml::node_view<const toml::node>& n) {
    if (auto s = n.value<std::string>()) return *s;
    if (auto i = n.value<int64_t>()) return std::to_string(*i);
    if (auto d = n.value<double>()) {
        std::ostringstream o;
        o << *d;
        return o.str();
    }
    if (auto b = n.value<bool>()) return *b ? "true" : "false";
    return {};
}

// "1.10.0" after "1.9.0": numbers compared as numbers, part by part
bool VersionLess(const std::string& a, const std::string& b) {
    std::istringstream x(a), y(b);
    std::string pa, pb;
    while (true) {
        bool ha = bool(std::getline(x, pa, '.')), hb = bool(std::getline(y, pb, '.'));
        if (!ha || !hb) return !ha && hb;
        long na = std::strtol(pa.c_str(), nullptr, 10), nb = std::strtol(pb.c_str(), nullptr, 10);
        if (na != nb) return na < nb;
        if (pa != pb) return pa < pb;
    }
}

bool ReadManifest(const fs::path& file, const std::string& game_id, PsxPackage& p) {
    toml::table t;
    try {
        t = toml::parse_file(U8(file));
    } catch (...) {
        return false;
    }
    bool meant = false;
    if (const toml::array* targets = t["target"].as_array()) {
        for (auto& e : *targets)
            if (const toml::table* tt = e.as_table()) {
                std::string id = (*tt)["game_id"].value_or(std::string());
                if (id == "*" || id == game_id) meant = true;
            }
    } else {
        meant = true;  // no targets: every game
    }
    if (!meant) return false;
    p.id = t["id"].value_or(std::string());
    p.version = t["version"].value_or(std::string());
    p.name = t["name"].value_or(p.id);
    p.author = t["author"].value_or(std::string());
    p.description = t["description"].value_or(std::string());
    if (const toml::array* fs_ = t["feature"].as_array())
        for (auto& e : *fs_)
            if (const toml::table* ft = e.as_table()) {
                PsxFeature f;
                f.id = (*ft)["id"].value_or(std::string());
                f.name = (*ft)["name"].value_or(f.id);
                f.description = (*ft)["description"].value_or(std::string());
                f.group = (*ft)["group"].value_or(std::string());
                f.default_enabled = (*ft)["default_enabled"].value_or(false);
                p.features.push_back(f);
            }
    if (const toml::array* os = t["option"].as_array())
        for (auto& e : *os)
            if (const toml::table* ot = e.as_table()) {
                PsxOption o;
                o.feature = (*ot)["feature"].value_or(std::string());
                o.id = (*ot)["id"].value_or(std::string());
                o.label = (*ot)["label"].value_or(o.id);
                o.description = (*ot)["description"].value_or(std::string());
                o.type = (*ot)["type"].value_or(std::string("string"));
                o.def = AsText(std::as_const(*ot)["default"]);
                o.min = (*ot)["min"].value_or(int64_t(0));
                o.max = (*ot)["max"].value_or(int64_t(100));
                o.step = std::max<int64_t>(1, (*ot)["step"].value_or(int64_t(1)));
                if (const toml::array* cs = (*ot)["choice"].as_array())
                    for (auto& c : *cs)
                        if (const toml::table* ct = c.as_table()) {
                            std::string v = AsText(std::as_const(*ct)["value"]);
                            o.choices.push_back({v, (*ct)["label"].value_or(v)});
                        }
                p.options.push_back(o);
            }
    return !p.id.empty();
}

}  // namespace

void PsxMods::Load(const fs::path& mods, const std::string& game_id) {
    dir_ = mods;
    packages_.clear();
    chosen_versions_.clear();
    state_.clear();
    error_.clear();
    loaded_ = true;
    std::error_code ec;
    // every package's versions; the newest kept (an installed one over a bundled one of the same version)
    std::map<std::string, PsxPackage> newest;
    for (const char* where : {"bundled", "installed"}) {
        fs::path root = mods / where;
        if (!fs::is_directory(root, ec)) continue;
        for (auto& pkg : fs::directory_iterator(root, ec)) {
            if (!pkg.is_directory(ec)) continue;
            for (auto& ver : fs::directory_iterator(pkg.path(), ec)) {
                PsxPackage p;
                if (!ReadManifest(ver.path() / "manifest.toml", game_id, p)) continue;
                p.installed = std::string(where) == "installed";
                auto it = newest.find(p.id);
                if (it == newest.end() || !VersionLess(p.version, it->second.version)) newest[p.id] = p;
            }
        }
    }
    for (auto& [id, p] : newest) packages_.push_back(p);
    // the game's own first, then the framework's; by name
    std::stable_sort(packages_.begin(), packages_.end(), [](const PsxPackage& a, const PsxPackage& b) {
        if (a.OfTheGame() != b.OfTheGame()) return a.OfTheGame();
        return a.name < b.name;
    });
    // the player's choices
    toml::table st;
    if (fs::exists(mods / "state.toml", ec)) {
        try {
            st = toml::parse_file(U8(mods / "state.toml"));
        } catch (const std::exception& e) {
            error_ = e.what();
        }
    }
    if (const toml::array* ps = st["package"].as_array())
        for (auto& e : *ps)
            if (const toml::table* pt = e.as_table())
                chosen_versions_.push_back({(*pt)["id"].value_or(std::string()), (*pt)["version"].value_or(std::string())});
    if (const toml::array* fs_ = st["feature"].as_array())
        for (auto& e : *fs_)
            if (const toml::table* ft = e.as_table()) {
                FeatureState s;
                s.package = (*ft)["package_id"].value_or(std::string());
                s.feature = (*ft)["id"].value_or(std::string());
                if (auto b = (*ft)["enabled"].value<bool>()) s.has_enabled = true, s.enabled = *b;
                if (const toml::table* vt = (*ft)["values"].as_table())
                    for (auto& [k, v] : *vt) s.values.push_back({std::string(k.str()), AsText(toml::node_view<const toml::node>(&v))});
                state_.push_back(s);
            }
}

const PsxMods::FeatureState* PsxMods::Find(const std::string& package, const std::string& feature) const {
    for (const FeatureState& s : state_)
        if (s.package == package && s.feature == feature) return &s;
    return nullptr;
}

PsxMods::FeatureState& PsxMods::State(const std::string& package, const std::string& feature) {
    for (FeatureState& s : state_)
        if (s.package == package && s.feature == feature) return s;
    state_.push_back({package, feature});
    return state_.back();
}

bool PsxMods::Enabled(const PsxPackage& p, const PsxFeature& f) const {
    const FeatureState* s = Find(p.id, f.id);
    return s && s->has_enabled ? s->enabled : f.default_enabled;
}

void PsxMods::SetEnabled(const PsxPackage& p, const PsxFeature& f, bool on) {
    FeatureState& s = State(p.id, f.id);
    s.has_enabled = true;
    s.enabled = on;
}

std::string PsxMods::Get(const PsxPackage& p, const PsxOption& o) const {
    if (const FeatureState* s = Find(p.id, o.feature))
        for (auto& [k, v] : s->values)
            if (k == o.id) return v;
    return o.def;
}

void PsxMods::Set(const PsxPackage& p, const PsxOption& o, const std::string& v) {
    FeatureState& s = State(p.id, o.feature);
    for (auto& [k, x] : s.values)
        if (k == o.id) {
            x = v;
            return;
        }
    s.values.push_back({o.id, v});
}

bool PsxMods::Save() {
    // the versions chosen: the ones there, and the version found for each package with a choice
    std::vector<std::pair<std::string, std::string>> versions = chosen_versions_;
    for (const FeatureState& s : state_) {
        bool listed = false;
        for (auto& [id, v] : versions) listed |= id == s.package;
        if (listed) continue;
        for (const PsxPackage& p : packages_)
            if (p.id == s.package) versions.push_back({p.id, p.version});
    }
    toml::table t;
    t.insert("format_version", 2);
    toml::array packages, features;
    for (auto& [id, v] : versions) packages.push_back(toml::table{{"id", id}, {"version", v}});
    for (const FeatureState& s : state_) {
        toml::table f{{"package_id", s.package}, {"id", s.feature}};
        if (s.has_enabled) f.insert("enabled", s.enabled);
        if (!s.values.empty()) {
            toml::table values;
            for (auto& [k, v] : s.values) values.insert(k, v);
            f.insert("values", values);
        }
        features.push_back(f);
    }
    if (!packages.empty()) t.insert("package", packages);
    if (!features.empty()) t.insert("feature", features);
    std::error_code ec;
    fs::create_directories(dir_, ec);
    fs::path file = dir_ / "state.toml", tmp = dir_ / "state.toml.tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << t << "\n";
        if (!o) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

}  // namespace launcher

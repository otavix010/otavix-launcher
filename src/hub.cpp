#include "hub.h"

#include <utility>

namespace launcher {

namespace {

Text ReadName(toml::node_view<const toml::node> n) {
    Text t;
    if (auto s = n.value<std::string>()) {
        t.pt = t.en = t.es = *s;
    } else if (const toml::table* tb = n.as_table()) {
        t.pt = (*tb)["pt"].value_or(std::string());
        t.en = (*tb)["en"].value_or(std::string());
        t.es = (*tb)["es"].value_or(std::string());
    }
    return t;
}

std::array<float, 3> ReadColor(toml::node_view<const toml::node> n, std::array<float, 3> def) {
    if (const toml::array* a = n.as_array(); a && a->size() == 3)
        for (size_t i = 0; i < 3; i++) def[i] = float((*a)[i].value_or(int64_t(128))) / 255.0f;
    return def;
}

fs::path Resolve(const fs::path& base, const std::string& p) {
    fs::path f = FromU8(p);
    return (f.is_absolute() ? f : base / f).make_preferred();
}

}  // namespace

bool HubConfig::Load(const fs::path& hub_dir, std::string& error) {
    toml::table t;
    fs::path file = hub_dir / "hub.toml";
    try {
        t = toml::parse_file(U8(file));
    } catch (const toml::parse_error& e) {
        error = std::string(e.description()) + " (" + U8(file) + ")";
        return false;
    }
    const auto& ct = std::as_const(t);
    name = ct["hub"]["name"].value_or(name);
    version = ct["hub"]["version"].value_or(version);
    update_repo = ct["hub"]["update_repo"].value_or(std::string());
    accent = ReadColor(ct["hub"]["accent"], accent);
    server_url = ct["servidor"]["url"].value_or(std::string());
    while (!server_url.empty() && server_url.back() == '/') server_url.pop_back();
    if (const toml::array* a = ct["libraries"].as_array())
        for (auto& e : *a) {
            auto v = toml::node_view<const toml::node>(&e);
            libraries.push_back({v["id"].value_or(std::string()), ReadName(v["name"]), ReadColor(v["accent"], {0.5f, 0.5f, 0.5f})});
        }
    if (const toml::array* a = ct["games"].as_array())
        for (auto& e : *a) {
            auto v = toml::node_view<const toml::node>(&e);
            GameEntry g;
            g.id = v["id"].value_or(std::string());
            g.library = v["library"].value_or(std::string());
            g.dir = Resolve(hub_dir, v["path"].value_or(std::string(".")));
            std::string prof = v["profile"].value_or(std::string());
            g.profile = prof.empty() ? g.dir / "launcher.toml" : Resolve(hub_dir, prof);
            g.on_windows = v["platforms"]["windows"].value_or(std::string());
            g.repo = v["repo"].value_or(std::string());
            g.on_linux = v["platforms"]["linux"].value_or(std::string());
            if (!g.id.empty()) games.push_back(std::move(g));
        }
    return true;
}

}  // namespace launcher

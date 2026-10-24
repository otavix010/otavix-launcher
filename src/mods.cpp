#include "mods.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <toml++/toml.hpp>

namespace launcher {

namespace {

Text ReadText(toml::node_view<const toml::node> n) {
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

std::vector<std::string> ActiveList(const fs::path& dir) {
    std::vector<std::string> out;
    std::ifstream in(dir / "ativos.txt", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::vector<Mod> ScanMods(const fs::path& dir) {
    std::vector<Mod> mods;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = U8(e.path().filename());
        if (name.empty() || name[0] == '_' || name[0] == '.') continue;
        Mod m;
        if (e.is_directory(ec)) {
            m.folder = name;
            m.name.pt = m.name.en = m.name.es = name;
            fs::path info = e.path() / "mod.toml";
            if (fs::is_regular_file(info, ec)) {
                try {
                    toml::table t = toml::parse_file(U8(info));
                    const auto& ct = std::as_const(t);
                    Text n = ReadText(ct["name"]);
                    if (!n.Empty()) m.name = n;
                    m.author = ct["author"].value_or(std::string());
                    m.version = ct["version"].value_or(std::string());
                    m.description = ReadText(ct["description"]);
                } catch (...) {
                }
            }
            for (auto& f : fs::recursive_directory_iterator(e.path(), ec)) {
                if (!f.is_regular_file(ec)) continue;
                std::string rel = U8(fs::relative(f.path(), e.path(), ec));
                std::replace(rel.begin(), rel.end(), '\\', '/');
                if (Lower(rel) == "mod.toml" || Lower(rel) == "leia-me.txt" || Lower(rel) == "readme.txt") continue;
                m.files.push_back(rel);
                m.bytes += f.file_size(ec);
            }
        } else if (Lower(U8(e.path().extension())) == ".zip") {
            m.folder = name;
            m.zip = true;
            m.name.pt = m.name.en = m.name.es = U8(e.path().stem());
            m.bytes = e.file_size(ec);
        } else {
            continue;
        }
        mods.push_back(std::move(m));
    }
    // active ones in their saved order, then the rest
    std::vector<std::string> active = ActiveList(dir);
    for (Mod& m : mods) m.active = !m.zip && std::find(active.begin(), active.end(), m.folder) != active.end();
    auto pos = [&](const Mod& m) {
        auto it = std::find(active.begin(), active.end(), m.folder);
        return m.active ? int(it - active.begin()) : m.zip ? 200000 : 100000;
    };
    std::stable_sort(mods.begin(), mods.end(), [&](const Mod& a, const Mod& b) {
        int pa = pos(a), pb = pos(b);
        return pa != pb ? pa < pb : Lower(a.folder) < Lower(b.folder);
    });
    return mods;
}

bool SaveActiveMods(const fs::path& dir, const std::vector<Mod>& mods) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path tmp = dir / "ativos.txt.tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << "# mods ativos, um por linha; o de cima vale quando dois trazem o mesmo arquivo\n";
        for (const Mod& m : mods)
            if (m.active && !m.zip) o << m.folder << "\n";
    }
    fs::rename(tmp, dir / "ativos.txt", ec);
    return !ec;
}

std::map<std::string, std::vector<std::string>> ModConflicts(const std::vector<Mod>& mods) {
    std::map<std::string, std::vector<std::string>> by_file;
    for (const Mod& m : mods)
        if (m.active)
            for (const std::string& f : m.files) by_file[Lower(f)].push_back(m.folder);
    std::map<std::string, std::vector<std::string>> out;
    for (auto& [f, holders] : by_file)
        if (holders.size() > 1) out[f] = holders;
    return out;
}

bool RemoveMod(const fs::path& dir, const Mod& mod, std::string& error) {
    std::error_code ec;
    fs::path bin = dir / "_removidos";
    fs::create_directories(bin, ec);
    fs::path to = bin / FromU8(mod.folder);
    for (int i = 2; fs::exists(to, ec); i++) to = bin / FromU8(mod.folder + " (" + std::to_string(i) + ")");
    fs::rename(dir / FromU8(mod.folder), to, ec);
    if (ec) error = ec.message();
    return !ec;
}

void FlattenMod(const fs::path& folder) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(folder, ec)) entries.push_back(e.path());
    if (entries.size() != 1 || !fs::is_directory(entries[0], ec)) return;
    // "data" and the like are the game's own folders: those stay
    std::string top = Lower(U8(entries[0].filename()));
    for (const char* own : {"data", "media", "movies", "audio", "sound", "video", "textures", "usrdir"})
        if (top == own) return;
    fs::path inner = entries[0];
    for (auto& e : fs::directory_iterator(inner, ec)) fs::rename(e.path(), folder / e.path().filename(), ec);
    fs::remove(inner, ec);
}

}  // namespace launcher

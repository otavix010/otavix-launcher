#include "profile.h"

#include <utility>

namespace launcher {

const std::string& Text::Get(Lang l) const {
    const std::string& t = l == Lang::Pt ? pt : l == Lang::Es ? es : en;
    if (!t.empty()) return t;
    return !en.empty() ? en : pt;
}

namespace {

Text ReadText(const toml::node_view<const toml::node> n) {
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

bool ReadValue(const toml::node* n, Value& out) {
    if (!n) return false;
    if (auto b = n->value<bool>(); b && n->is_boolean()) {
        out = *b;
    } else if (n->is_integer()) {
        out = *n->value<int64_t>();
    } else if (n->is_floating_point()) {
        out = *n->value<double>();
    } else if (n->is_string()) {
        out = *n->value<std::string>();
    } else {
        return false;
    }
    return true;
}

OptType ReadType(const std::string& s) {
    if (s == "toggle") return OptType::Toggle;
    if (s == "slider") return OptType::Slider;
    if (s == "resolution") return OptType::Resolution;
    if (s == "buttons") return OptType::ButtonMap;
    if (s == "path") return OptType::Path;
    return OptType::Choice;
}

Target ReadTarget(const std::string& s) {
    if (s == "env") return Target::Env;
    if (s == "launcher") return Target::Launcher;
    if (s == "ini") return Target::Ini;
    return Target::Settings;
}

}  // namespace

bool Profile::Load(const fs::path& file, std::string& error) {
    toml::table t;
    try {
        t = toml::parse_file(U8(file));
    } catch (const toml::parse_error& e) {
        error = std::string(e.description()) + " (" + U8(file) + ")";
        return false;
    }
    const toml::node_view<const toml::node> g = std::as_const(t)["game"];
    title = ReadText(g["title"]);
    subtitle = ReadText(g["subtitle"]);
    legal = ReadText(g["legal"]);
    status = g["status"].value_or(status);
    status_text = ReadText(g["status_text"]);
    perf_note = ReadText(std::as_const(t)["performance"]["note"]);
    mods_folder = std::as_const(t)["mods"]["folder"].value_or(std::string());
    mods_kind = std::as_const(t)["mods"]["kind"].value_or(std::string());
    mods_game_id = std::as_const(t)["mods"]["game_id"].value_or(std::string());
    mods_help = ReadText(std::as_const(t)["mods"]["help"]);
    install.check = std::as_const(t)["install"]["check"].value_or(std::string());
    install.pkg = std::as_const(t)["install"]["pkg"].value_or(std::string());
    install.sha1 = std::as_const(t)["install"]["sha1"].value_or(std::string());
    install.version = ReadText(std::as_const(t)["install"]["version"]);
    install.folder = std::as_const(t)["install"]["folder"].value_or(std::string("jogo"));
    if (const toml::array* f = std::as_const(t)["install"]["files"].as_array())
        for (auto& e : *f)
            if (auto v = e.value<std::string>()) install.files.push_back(*v);
    install.help = ReadText(std::as_const(t)["install"]["help"]);
    exe = g["exe"].value_or(std::string());
    if (const toml::array* a = g["exe"].as_array())
        for (auto& e : *a)
            if (auto s = e.value<std::string>()) exes.push_back(*s);
    if (!exes.empty()) exe = exes[0];
    workdir = g["workdir"].value_or(std::string());
    exe_workdir = workdir == "exe";
    if (const toml::table* e = g["env"].as_table())
        for (auto& [k, v] : *e)
            if (auto s = v.value<std::string>()) env.push_back({std::string(k.str()), *s});
    log_file = g["log_file"].value_or(std::string());
    if (const toml::array* a = g["report_files"].as_array())
        for (auto& e : *a)
            if (auto s = e.value<std::string>()) report_files.push_back(*s);
    port_file = g["port_file"].value_or(std::string());
    const toml::node_view<const toml::node> in = std::as_const(t)["install"];
    install.kind = in["kind"].value_or(std::string());
    install.done = in["done"].value_or(std::string());
    install.marks = in["marks"].value_or(std::string());
    install.needs = in["needs"].value_or(std::string());
    install.time = ReadText(in["time"]);
    for (const char* list : {"command", "redo"})
        if (const toml::array* a = in[list].as_array())
            for (auto& e : *a)
                if (auto s = e.value<std::string>()) (std::string(list) == "command" ? install.command : install.redo).push_back(*s);
    if (const toml::array* a = in["inputs"].as_array())
        for (auto& e : *a)
            if (const toml::table* it = e.as_table()) {
                BuildInput b;
                b.key = (*it)["key"].value_or(std::string());
                b.label = ReadText((*it)["label"]);
                b.help = ReadText((*it)["help"]);
                b.auto_text = ReadText((*it)["auto"]);
                b.unpack = (*it)["unpack"].value_or(std::string());
                std::string pick = (*it)["pick"].value_or(std::string("folder"));
                b.folder = pick != "file";
                b.file = pick != "folder";
                if (const toml::array* f = (*it)["filters"].as_array())
                    for (auto& x : *f)
                        if (auto s = x.value<std::string>()) b.filters.push_back(*s);
                if (const toml::array* l = (*it)["links"].as_array())
                    for (auto& x : *l)
                        if (const toml::table* t = x.as_table())
                            b.links.push_back({ReadText((*t)["label"]), (*t)["url"].value_or(std::string())});
                if (const toml::table* d = (*it)["download"].as_table()) {
                    b.download_url = (*d)["url"].value_or(std::string());
                    b.download_sha1 = (*d)["sha1"].value_or(std::string());
                    b.download_file = (*d)["file"].value_or(std::string());
                    b.download_size = uint64_t((*d)["size"].value_or(int64_t(0)));
                    b.download_label = ReadText((*d)["label"]);
                }
                install.inputs.push_back(b);
            }
    if (const toml::array* a = in["steps"].as_array())
        for (auto& e : *a)
            if (const toml::table* it = e.as_table()) install.steps.push_back({(*it)["id"].value_or(std::string()), ReadText((*it)["label"])});
    if (const toml::array* a = in["prereqs"].as_array())
        for (auto& e : *a)
            if (const toml::table* it = e.as_table()) {
                Prereq p;
                p.name = ReadText((*it)["name"]);
                p.help = ReadText((*it)["help"]);
                p.check = (*it)["check"].value_or(std::string());
                p.install = (*it)["install"].value_or(std::string());
                p.output = (*it)["output"].value_or(false);
                install.prereqs.push_back(p);
            }
    settings = g["settings"].value_or(settings);
    ini = g["ini"].value_or(std::string());
    ini_template = g["ini_template"].value_or(std::string());
    logs = g["logs"].value_or(logs);
    game_file = g["game_file"].value_or(std::string());
    game_option = g["game_option"].value_or(std::string());
    exe_option = g["exe_option"].value_or(std::string());
    if (const toml::array* a = g["search"].as_array())
        for (auto& e : *a)
            if (auto s = e.value<std::string>()) search.push_back(*s);
    if (const toml::array* a = g["args"].as_array())
        for (auto& e : *a)
            if (auto s = e.value<std::string>()) args.push_back(*s);
    if (const toml::array* a = g["saves"].as_array())
        for (auto& e : *a)
            if (auto s = e.value<std::string>()) saves.push_back(*s);
    if (const toml::array* a = g["accent"].as_array(); a && a->size() == 3)
        for (size_t i = 0; i < 3; i++) accent[i] = float((*a)[i].value_or(int64_t(128))) / 255.0f;
    if (exe.empty()) {
        error = "launcher.toml: [game] exe is missing";
        return false;
    }

    if (const toml::array* a = t["pages"].as_array())
        for (auto& e : *a)
            if (const toml::table* p = e.as_table())
                pages.push_back({(*p)["id"].value_or(std::string()), ReadText((*p)["label"]), (*p)["advanced"].value_or(false)});

    if (const toml::array* a = t["options"].as_array())
        for (auto& e : *a) {
            const toml::table* o = e.as_table();
            if (!o) continue;
            auto v = toml::node_view<const toml::node>(o);
            Option op;
            op.page = v["page"].value_or(std::string());
            op.key = v["key"].value_or(std::string());
            op.type = ReadType(v["type"].value_or(std::string("choice")));
            op.target = ReadTarget(v["target"].value_or(std::string("settings")));
            op.label = ReadText(v["label"]);
            op.help = ReadText(v["help"]);
            ReadValue(v["default"].node(), op.def);
            op.min = v["min"].value_or(op.min);
            op.max = v["max"].value_or(op.max);
            op.step = v["step"].value_or(op.step);
            op.suffix = v["suffix"].value_or(std::string());
            op.width_key = v["width_key"].value_or(op.width_key);
            op.height_key = v["height_key"].value_or(op.height_key);
            op.guest_height = v["guest_height"].value_or(int64_t(0));
            if (const toml::array* c = v["choices"].as_array())
                for (auto& ce : *c) {
                    auto cv = toml::node_view<const toml::node>(&ce);
                    ChoiceItem item;
                    ReadValue(cv["value"].node(), item.value);
                    item.label = ReadText(cv["label"]);
                    if (const toml::table* st = cv["set"].as_table())
                        for (auto& [k, n] : *st) {
                            Value sv;
                            if (ReadValue(&n, sv)) item.set.push_back({std::string(k.str()), sv});
                        }
                    op.choices.push_back(std::move(item));
                }
            if (const toml::array* b = v["buttons"].as_array())
                for (auto& be : *b) {
                    auto bv = toml::node_view<const toml::node>(&be);
                    op.buttons.push_back({bv["key"].value_or(std::string()), ReadText(bv["label"]),
                                          bv["default"].value_or(std::string())});
                }
            op.pick = v["pick"].value_or(op.pick);
            if (const toml::array* f = v["filters"].as_array())
                for (auto& fe : *f)
                    if (auto fs = fe.value<std::string>()) op.filters.push_back(*fs);
            if (op.type == OptType::ButtonMap && op.key.empty()) op.key = "buttons";
            if (op.type == OptType::Path && !std::holds_alternative<std::string>(op.def)) op.def = std::string();
            options.push_back(std::move(op));
        }

    if (const toml::array* a = t["presets"].as_array())
        for (auto& e : *a) {
            auto pv = toml::node_view<const toml::node>(&e);
            Preset p;
            p.label = ReadText(pv["label"]);
            p.help = ReadText(pv["help"]);
            if (const toml::table* vals = pv["values"].as_table())
                for (auto& [k, n] : *vals) {
                    Value val;
                    if (ReadValue(&n, val)) p.values.push_back({std::string(k.str()), val});
                }
            presets.push_back(std::move(p));
        }

    if (const toml::table* d = t["defaults"].as_table())
        for (auto& [k, n] : *d) {
            Value v;
            if (ReadValue(&n, v)) defaults.push_back({std::string(k.str()), v});
        }

    if (const toml::array* a = t["credits"].as_array())
        for (auto& e : *a) {
            auto cv = toml::node_view<const toml::node>(&e);
            credits.push_back({ReadText(cv["name"]), ReadText(cv["what"])});
        }
    return true;
}

const Value* Profile::DefaultFor(const std::string& key) const {
    for (auto& [k, v] : defaults)
        if (k == key) return &v;
    return nullptr;
}

const Option* Profile::Find(const std::string& key) const {
    for (const Option& o : options)
        if (o.key == key) return &o;
    return nullptr;
}

}  // namespace launcher

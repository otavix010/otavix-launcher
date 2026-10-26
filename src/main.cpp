// Otavix Launcher: a hub for the native ports. hub.toml lists the libraries and each port's folder; each port's
// launcher.toml says what its page shows, and the choices go to that game's settings file, its environment and the
// launcher's own state. Mouse, keyboard and controller all work.
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "art.h"
#include "theme.h"
#include "catalog.h"
#include "closegame.h"
#include "desempenho.h"
#include "hub.h"
#include "icon.h"
#include "build.h"
#include "install.h"
#include "i18n.h"
#include "mods.h"
#include "notas.h"
#include "music.h"
#include "ports.h"
#include "net.h"
#include "photo.h"
#include "profile.h"
#include "psxmods.h"
#include "saves.h"
#include "secret.h"
#include "shortcut.h"
#include "store.h"
#include "sysinfo.h"
#include "update.h"

using namespace launcher;

namespace {

// ---------------------------------------------------------------------------------------------- state

using Art = std::vector<std::pair<SDL_Texture*, ImVec2>>;  // texture, size

// one port: its profile, its settings, its art, its saves
struct Game {
    GameEntry entry;
    Profile prof;
    bool ok = false;
    std::string error;
    Store store;
    ArtSpec art_spec;
    ThemeSpec theme_spec;
    IconSpec icon_spec;
    GameIcon icon;  // its window's and its shortcut's
    bool theme_tried = false;  // the theme from the game was tried, this session or since its install
    Art art;
    std::mutex art_m;
    std::vector<ArtImage> art_loaded;
    bool art_ready = false;
    std::vector<Backup> backups;
    bool backups_stale = true;
    PsxMods psx_mods;  // [mods] kind = "psxrecomp": the packages and the player's choices
    int confirm_restore = -1;
    bool crashed = false;  // the last session ended with an error, not yet reported or dismissed
    Installer installer;
    PortFetcher fetcher;  // the port's own files, from its releases
    bool port_checked = false;  // its newest release looked up this session
    bool install_taken = true;  // the finished install's game is already the one to play
    bool confirm_delete_archive = false;
    bool manual_open = false;  // the manual install's steps shown
    GameBuild build;           // [install] kind = "build": the generator making the game on this PC
    std::mutex prereq_m;
    std::vector<int> prereq;   // each tool: -1 being checked, 0 missing, 1 there
    bool prereq_asked = false;
    bool build_noted = false;  // the finished build's version is kept
    bool build_form = false;   // the folders asked again, to make it again
    // a build input being unpacked first (an archive or a disc image picked): its key, whether the build is a redo,
    // a disc image found inside the archive (unpacked in turn, then deleted), and why the last unpacking failed
    std::string unpack_key, unpack_image, unpack_error;
    bool unpack_redo = false;
    std::vector<Mod> mods;
    bool mods_stale = true;
    std::atomic<bool> mods_busy = false;
    std::string mods_msg;
    int confirm_remove = -1;
    int page = 0, tab = 0;
    int last_exit = INT_MIN;
    double last_minutes = 0;

    // ok, beta (playable, in development), wip or soon on that system; a port that names no system is playable here only
    std::string StateOn(Platform p, Platform here) const {
        if (entry.on_windows.empty() && entry.on_linux.empty()) return p == here ? prof.status : "";
        return entry.On(p);
    }
};

// the launcher's own settings, in hub_state.toml
class HubState {
public:
    void Open(const fs::path& f) {
        file_ = f;
        std::error_code ec;
        if (fs::exists(f, ec)) try {
                t_ = toml::parse_file(U8(f));
            } catch (...) {
            }
    }
    std::string Str(const char* k, const std::string& d) const { return t_[k].value_or(d); }
    int64_t Int(const char* k, int64_t d) const { return t_[k].value_or(d); }
    bool Bool(const char* k, bool d) const { return t_[k].value_or(d); }
    template <class T>
    void Set(const char* k, const T& v) {
        t_.insert_or_assign(k, v);
        Save();
    }
    void Remove(const char* k) {
        if (t_.erase(k)) Save();
    }

private:
    void Save() {
        fs::path tmp = file_;
        tmp += ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) return;
            o << t_ << "\n";
        }
        std::error_code ec;
        fs::rename(tmp, file_, ec);
    }
    fs::path file_;
    toml::table t_;
};

enum class Screen { Hub, Game };

struct App {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    fs::path hub_dir;
    HubConfig hub;
    bool hub_ok = false;
    std::string hub_error;
    HubState state;
    Lang lang = Lang::En;
    float content_scale = 1, scale = 1;
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* title = nullptr;
    std::vector<std::unique_ptr<Game>> games;
    Screen screen = Screen::Hub;
    int entry = 0;     // the hub's sidebar: all games, each library, then the launcher's settings
    int cur = -1;      // the open game
    int settings_tab = 0;
    bool focus_page = true;  // the page just changed: its first item takes the focus
    bool focus_tab = false;
    std::string help;        // the hovered or focused option's help
    // the game being played
    SDL_Process* proc = nullptr;
    int proc_game = -1;
    Uint64 started = 0;
    // a short message at the bottom
    std::string toast;
    Uint64 toast_until = 0;
    // a ButtonMap entry waiting for a pad button
    const Option* cap_opt = nullptr;
    const ButtonSlot* cap_slot = nullptr;
    // a file dialog's answer, from its thread
    std::mutex dlg_m;
    std::string dlg_path, dlg_key;
    int dlg_game = -1;
    bool dlg_ready = false;
    // the primary monitor's modes, pixels
    std::vector<std::pair<int, int>> modes;
    Updater updater;
    ReleaseNotes notes;  // the launcher's and the ports' release notes, looked up when first shown
    Uint64 art_t0 = 0;
    bool quit = false;
#ifdef _WIN32
    Platform here = Platform::Windows;
#else
    Platform here = Platform::Linux;
#endif
    Platform view = here;  // the version shown on the open game's page
    Music music;
    std::atomic<bool> theme_made{false};  // a theme from a game is ready: the page plays it
    SDL_IOStream* proc_log = nullptr;  // the game's output, for a profile's log_file
    bool closing = false;  // the player closed the game from the controller: its exit is no error
    std::string install_id;  // random, made on the first start: one vote per game and computer
    // the server: port requests and their rankings
    Call rank_call, vote_call;
    std::vector<std::vector<std::string>> ranking;  // console, key, name, votes
    int rank_period = 0, rank_console = -1;
    bool rank_stale = true;
    std::string rank_error, req_msg;
    char req_name[96] = "", req_comment[300] = "";
    int req_console = 4;
    Catalogs catalogs;
    Covers covers;
    const CatalogGame* req_pick = nullptr;  // the game chosen from the catalog
    bool req_free = false;                  // a name the catalog does not have, as typed
    // the hub's search bar: what was typed, and the hits for it
    char search[96] = "";
    std::string search_done;                // the search the hits below are for
    std::vector<int> search_games;          // the hub's games that match
    std::vector<std::pair<std::string, const CatalogGame*>> search_catalog;  // console, game: no port yet
    size_t search_capped = 0;               // more catalog hits than shown: how many are shown
    Call search_vote;
    std::string search_vote_name, search_vote_id;
    std::set<std::string> search_voted;     // "console/key" requested from the search in this run
    bool focus_search = false;
    int search_filter = 0;                  // the hub's games shown: 0 all, 1 playable here, 2 in development, 3 installed
    // the report being written
    Call report_call;
    int rep_cat = 0;
    char rep_desc[4000] = "", rep_contact[160] = "";
    bool rep_attach = true;
    std::string rep_msg;
    std::vector<Photo> rep_photos;              // up to 4, made JPEG when added
    std::vector<std::string> rep_photo_picked;  // files from the dialog, taken on the frame (under dlg_m)
    std::string rep_photo_msg;
    // the pictures go after the report, a request each, to the id it got
    Call photo_call;
    std::string photo_report;
    std::vector<Photo> photo_queue;
    size_t photo_next = 0, photo_failed = 0;
    // the player's own port requests, to take one back
    Call mine_call, withdraw_call;
    std::vector<std::vector<std::string>> mine;  // console, key, name, when, comment, cover
    bool mine_stale = true;
    std::vector<std::pair<std::string, std::string>> sysinfo;
    // performance: the file the running game writes, the first session's consent, the server's answers
    fs::path perf_file;
    bool perf_ask = false;    // a measured session ended and the player was never asked about sharing
    Session perf_pending;     // that session, sent if the player agrees
    Call perf_call, perf_sum_call, perf_del_call;
    std::string perf_sum_game;                        // the game the community's rows are about
    std::vector<std::vector<std::string>> perf_rows;  // gpu, cpu, summary, sessions, computers, fps avg, 1%, min
    std::string perf_msg;
    // the launcher's and the installed ports' newest versions: looked up on start and every 6 hours, each notice
    // shown once a look
    Uint64 last_check = 0;
    bool upd_shown = false, ports_shown = false;
    // the owner's administration
    Call admin_call, admin_rank_call, admin_reports_call;
    std::vector<std::vector<std::string>> admin_ranking, admin_reports;  // console, key, name, votes, hidden, comments
    int admin_period = 2;
    bool admin_stale = true;
    std::string admin_msg, adm_console, adm_key;
    char adm_name[96] = "", admin_key[160] = "";
    std::string admin_token;  // the server's key, kept sealed by Windows in hub_state.toml
    char adm_reply[1024] = "";  // the owner's answer being written to a report, and that report's id
    std::string adm_reply_id;
    // a report's attachment being fetched to open, and a request about to be removed for good
    Call admin_file_call;
    std::string admin_file_name, adm_remove_console, adm_remove_key, adm_remove_name;
    // the owner's answers to the reports sent from this computer: id, game, title, kind, the report's start, the
    // answer, when; those not seen yet are shown in a pop-up
    Call answers_call;
    std::vector<std::vector<std::string>> answers;
    std::vector<size_t> answers_new;
    // a mod .zip picked in the file dialog
    std::string zip_path;
    bool zip_ready = false;
    // the download picked to install a game
    std::string inst_path;
    bool inst_ready = false;
    int inst_game = -1;
};

App g;

Game& G() { return *g.games[size_t(g.cur)]; }

bool Playable(const Game& gm, Platform p) {
    const std::string st = gm.StateOn(p, g.here);
    return gm.ok && p == g.here && (st == "ok" || st == "beta");
}

const char* PlatformName(Platform p) { return p == Platform::Windows ? "Windows" : "Linux/SteamOS"; }

// green playable, amber in development (beta too: made and played, but still in development), grey coming
ImU32 StateColor(const std::string& st) {
    return st == "ok" ? IM_COL32(80, 200, 110, 255) : st == "wip" || st == "beta" ? IM_COL32(230, 170, 50, 255) : IM_COL32(120, 120, 130, 255);
}

// a card's or page's word for a port's state on a system: empty when it is playable here
std::string StateLabel(const Game& gm, Platform p) {
    std::string st = gm.StateOn(p, g.here);
    if (st == "ok") return p == g.here ? std::string() : std::string(T(g.lang, S::Available));
    // beta: playable here, and the card still says it is in development
    if (st == "wip" || st == "beta") return T(g.lang, S::Wip);
    if (st == "soon") return T(g.lang, S::Soon);
    return gm.prof.status_text.Empty() ? std::string(T(g.lang, S::Wip)) : gm.prof.status_text.Get(g.lang);
}

const char* Tr(S s) { return T(g.lang, s); }
const char* Tx(const Text& t) { return t.Get(g.lang).c_str(); }

void Toast(const std::string& s, Uint64 ms = 3500) {
    g.toast = s;
    g.toast_until = SDL_GetTicks() + ms;
}

// one colour for the launcher's chrome everywhere; a game's own colour only tints its art
std::array<float, 3> CurAccent() { return g.hub.accent; }

ImVec4 AccentOf(const std::array<float, 3>& a, float mul = 1, float alpha = 1) {
    return ImVec4(std::min(1.0f, a[0] * mul), std::min(1.0f, a[1] * mul), std::min(1.0f, a[2] * mul), alpha);
}
ImVec4 Accent(float mul = 1, float alpha = 1) { return AccentOf(CurAccent(), mul, alpha); }

const Library* FindLibrary(const std::string& id) {
    for (const Library& l : g.hub.libraries)
        if (l.id == id) return &l;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------- look

void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 10;
    s.FrameRounding = 8;
    s.PopupRounding = 8;
    s.GrabRounding = 8;
    s.ScrollbarRounding = 8;
    s.FramePadding = ImVec2(14, 9);
    s.ItemSpacing = ImVec2(12, 10);
    s.WindowPadding = ImVec2(0, 0);
    s.ScrollbarSize = 12;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.93f, 0.94f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.56f, 0.6f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.07f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.1f, 0.1f, 0.115f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.11f, 0.13f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.19f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.21f, 0.25f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.3f, 1);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.21f, 1);
    c[ImGuiCol_ButtonHovered] = Accent(0.8f);
    c[ImGuiCol_ButtonActive] = Accent(1.0f);
    c[ImGuiCol_Header] = Accent(0.55f, 0.55f);
    c[ImGuiCol_HeaderHovered] = Accent(0.7f, 0.7f);
    c[ImGuiCol_HeaderActive] = Accent(0.9f, 0.9f);
    c[ImGuiCol_CheckMark] = Accent(1.2f);
    c[ImGuiCol_SliderGrab] = Accent(1.0f);
    c[ImGuiCol_SliderGrabActive] = Accent(1.2f);
    c[ImGuiCol_NavCursor] = Accent(1.3f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.14f, 0.92f);
    c[ImGuiCol_TabHovered] = Accent(0.75f);
    c[ImGuiCol_TabSelected] = Accent(0.6f);
    c[ImGuiCol_TabSelectedOverline] = Accent(1.2f);
    c[ImGuiCol_TabDimmed] = ImVec4(0.12f, 0.12f, 0.14f, 0.92f);
    c[ImGuiCol_TabDimmedSelected] = Accent(0.45f);
    c[ImGuiCol_TabDimmedSelectedOverline] = Accent(0.9f);
    s.ScaleAllSizes(g.scale);
    s.FontScaleDpi = g.scale;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    fs::path f = g.hub_dir / "launcher" / "fonts";
    auto load = [&](const char* name, float size) -> ImFont* {
        std::string p = U8(f / name);
        ImFont* font = io.Fonts->AddFontFromFileTTF(p.c_str(), size);
        return font ? font : io.Fonts->AddFontDefault();
    };
    g.body = load("Inter-Regular.ttf", 19);
    g.bold = load("Inter-SemiBold.ttf", 19);
    g.title = load("InterDisplay-Black.ttf", 64);
}

// the launcher's scale: the monitor's, times the interface scale setting
void ApplyScale() {
    g.scale = g.content_scale * float(std::clamp<int64_t>(g.state.Int("ui_scale", 100), 70, 160)) / 100.0f;
    ApplyTheme();
}

// the controller's button as a small round badge, then its word
void Hint(const char* button, ImU32 color, const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // a circle for one letter, a pill for two
    float r = ImGui::GetFontSize() * 0.62f;
    ImVec2 ts = ImGui::CalcTextSize(button);
    float w = std::max(r * 2, ts.x + r);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float cy = p.y + ImGui::GetFontSize() * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, cy - r), ImVec2(p.x + w, cy + r), color, r);
    dl->AddText(ImVec2(p.x + (w - ts.x) / 2, cy - ts.y / 2), IM_COL32(255, 255, 255, 255), button);
    ImGui::Dummy(ImVec2(w, ImGui::GetFontSize()));
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
}

void Heading(const char* text) {
    ImGui::PushFont(g.bold, 30);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Spacing();
}

void Card(const char* id, float height, void (*fill)()) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.13f, 0.92f));
    ImGui::BeginChild(id, ImVec2(-1, height), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    fill();
    ImGui::EndChild();
}

void Dim(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------- game files

std::string GamePath(Game& gm) {
    const Option* o = gm.prof.game_option.empty() ? nullptr : gm.prof.Find(gm.prof.game_option);
    if (!o) return {};
    Value v = gm.store.Get(*o);
    auto s = std::get_if<std::string>(&v);
    return s ? *s : std::string();
}

// where the game is: the chosen folder or image, else a search folder beside the port or above it; empty when nothing
// holds the game's file
// a made game whose copy of the game is gone: the file it reads is missing (the folder or file picked was moved,
// deleted, or is on a drive that is off); picked again, the generator remakes its link in seconds. needs is relative
// to the port's folder ("vfs/PS3_GAME/PARAM.SFO", through the generator's link) or an input's path ("{iso}")
bool CopyMissing(const Game& gm) {
    const InstallSpec& in = gm.prof.install;
    std::error_code ec;
    if (in.kind != "build" || in.needs.empty() || !fs::is_regular_file(gm.entry.dir / FromU8(in.done), ec)) return false;
    std::string n = in.needs;
    for (const BuildInput& b : in.inputs)
        for (size_t at; (at = n.find("{" + b.key + "}")) != std::string::npos;) n.replace(at, b.key.size() + 2, gm.store.GetOwn(b.key));
    if (n.empty()) return false;
    fs::path p = FromU8(n);
    return !fs::is_regular_file(p.is_absolute() ? p : gm.entry.dir / p, ec);
}

fs::path FindGame(Game& gm) {
    std::error_code ec;
    if (gm.prof.install.kind == "build") {
        if (!fs::is_regular_file(gm.entry.dir / FromU8(gm.prof.install.done), ec)) return {};
        return gm.prof.game_file.empty() ? gm.entry.dir : gm.entry.dir / FromU8(gm.prof.game_file);
    }
    std::string chosen_path = GamePath(gm);
    auto holds = [&](const fs::path& p) {
        if (fs::is_regular_file(p, ec)) return true;  // a disc image or the file itself
        return gm.prof.game_file.empty() ? fs::is_directory(p, ec) : fs::is_regular_file(p / FromU8(gm.prof.game_file), ec);
    };
    auto has_image = [&](const fs::path& d) -> fs::path {
        for (auto& e : fs::directory_iterator(d, ec)) {
            std::string ext = U8(e.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (ext == ".iso" && e.is_regular_file(ec)) return e.path();
        }
        return {};
    };
    if (!chosen_path.empty()) {
        fs::path p = FromU8(chosen_path);
        return holds(p) ? p : fs::path();
    }
    if (!gm.prof.install.folder.empty()) {
        fs::path own = gm.entry.dir / FromU8(gm.prof.install.folder);
        if (fs::is_directory(own, ec)) {
            if (!gm.prof.install.files.empty()) {
                for (auto& e : fs::directory_iterator(own, ec))
                    if (e.is_regular_file(ec) && IsGameFile(e.path(), gm.prof.install)) return e.path();
            } else {
                if (holds(own)) return own;
                if (fs::path img = has_image(own); !img.empty()) return img;
            }
        }
    }
    fs::path d = gm.entry.dir;
    for (int i = 0; i < 6; i++) {
        for (const std::string& s : gm.prof.search) {
            fs::path c = d / FromU8(s);
            if (!fs::is_directory(c, ec)) continue;
            if (holds(c)) return c;
            if (fs::path img = has_image(c); !img.empty()) return img;
        }
        if (!d.has_parent_path() || d.parent_path() == d) break;
        d = d.parent_path();
    }
    return {};
}

void DialogDone(void*, const char* const* files, int) {
    std::lock_guard lock(g.dlg_m);
    if (files && files[0]) {
        g.dlg_path = files[0];
        g.dlg_ready = true;
    }
}

void PickPath(const Option& o, bool folder) {
    {
        std::lock_guard lock(g.dlg_m);
        g.dlg_key = o.key;
        g.dlg_game = g.cur;
        g.dlg_ready = false;
    }
    std::string start = U8(G().entry.dir);
    if (folder) {
        SDL_ShowOpenFolderDialog(DialogDone, nullptr, g.win, start.c_str(), false);
    } else {
        static std::vector<SDL_DialogFileFilter> filters;
        static std::vector<std::string> patterns;
        filters.clear();
        patterns.clear();
        std::string all;
        for (const std::string& f : o.filters) all += (all.empty() ? "" : ";") + f;
        patterns.push_back(all.empty() ? "*" : all);
        filters.push_back({Tx(o.label), patterns.back().c_str()});
        SDL_ShowOpenFileDialog(DialogDone, nullptr, g.win, filters.data(), int(filters.size()), start.c_str(), false);
    }
}

void StartArt(Game& gm);

void TakeDialog() {
    std::lock_guard lock(g.dlg_m);
    if (!g.dlg_ready) return;
    g.dlg_ready = false;
    if (g.dlg_game < 0 || g.dlg_game >= int(g.games.size())) return;
    Game& gm = *g.games[size_t(g.dlg_game)];
    if (const Option* o = gm.prof.Find(g.dlg_key)) {
        gm.store.Set(*o, g.dlg_path);
        gm.store.Save();
        StartArt(gm);
    }
}

void OpenPath(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    std::string url = "file:///" + U8(p);
    std::replace(url.begin(), url.end(), '\\', '/');
    SDL_OpenURL(url.c_str());
}

// ---------------------------------------------------------------------------------------------- the game's process

std::string Expand(Game& gm, const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{') {
            size_t e = s.find('}', i);
            if (e != std::string::npos) {
                std::string key = s.substr(i + 1, e - i - 1);
                if (key == "game") out += U8(FindGame(gm));
                else if (key == "dir") out += U8(gm.entry.dir);
                else if (const Option* o = gm.prof.Find(key)) out += ToText(gm.store.Get(*o));
                else
                    for (const BuildInput& b : gm.prof.install.inputs)  // what the player pointed at when making it
                        if (b.key == key) out += gm.store.GetOwn(key);
                i = e;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// the open game's theme, when the setting allows and no game is running
// the theme the port's profile makes from the player's game, once, in the background: PlayTheme runs again when done
void MakeGameTheme(Game& gm, const fs::path& dir) {
    if (gm.theme_spec.Empty() || gm.theme_tried) return;
    fs::path game = FindGame(gm);
    if (game.empty()) return;
    gm.theme_tried = true;
    std::thread([spec = gm.theme_spec, game, wav = dir / "tema.wav"] {
        if (MakeTheme(spec, game, wav)) g.theme_made = true;
    }).detach();
}

void PlayTheme() {
    if (g.cur < 0 || g.screen != Screen::Game || g.proc || !g.state.Bool("music", true)) return;
    fs::path dir = g.hub_dir / "arte" / FromU8(G().entry.id);
    fs::path f = FindTheme(dir);
    if (f.empty()) {
        g.music.Stop();
        MakeGameTheme(G(), dir);
    } else {
        g.music.Play(f);
    }
}

// the executable that runs, relative to the port: the profile's exe (or the option that picks it), else the first
// of exe = [...] that exists (one build per processor kind)
std::string GameExe(Game& gm) {
    std::string exe = gm.prof.exe;
    if (const Option* o = gm.prof.exe_option.empty() ? nullptr : gm.prof.Find(gm.prof.exe_option))
        exe = ToText(gm.store.Get(*o));
    std::error_code ec;
    for (const std::string& e : gm.prof.exes)
        if (fs::is_regular_file(gm.entry.dir / FromU8(e), ec)) {
            exe = e;
            break;
        }
    return exe;
}

void Launch(int index) {
    if (g.proc) return;
    Game& gm = *g.games[size_t(index)];
    if (!Playable(gm, g.here)) return;
    // the copy of the game gone: the game would open and quit; its page says what to do
    if (CopyMissing(gm)) {
        Toast(Tr(S::BuildCopyMissing), 6000);
        return;
    }
    if (gm.store.Dirty() && !gm.store.Save()) Toast(Tr(S::SaveFailed));
    if (gm.store.GetOwnBool("auto_backup", true)) {
        std::string err;
        if (MakeBackup(gm.entry.dir, gm.prof, "auto", err)) PruneAutomatic(gm.entry.dir, 10);
        gm.backups_stale = true;
    }
    std::string exe = GameExe(gm);
    std::error_code ec;
    fs::path exe_path = (gm.entry.dir / FromU8(exe)).make_preferred();
    std::vector<std::string> args{U8(exe_path)};
    for (const std::string& a : gm.prof.args) {
        std::string v = Expand(gm, a);
        // an option standing alone that is only a flag ("--windowed" or nothing) leaves no empty argument behind
        if (v.empty() && a.size() > 2 && a.front() == '{' && a.back() == '}' && a != "{game}") continue;
        if (a.find("{dir}") != std::string::npos) v = U8(FromU8(v).make_preferred());  // a path in the port: \ throughout
        args.push_back(v);
    }
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    SDL_Environment* env = SDL_CreateEnvironment(true);
    for (auto& [k, v] : gm.store.Environment(gm.prof)) SDL_SetEnvironmentVariable(env, k.c_str(), v.c_str(), true);
    if (!g.state.Bool("mods_on", true)) SDL_SetEnvironmentVariable(env, "OTAVIX_MODS", "0", true);
    for (auto& [k, v] : gm.prof.env) SDL_SetEnvironmentVariable(env, k.c_str(), U8(FromU8(Expand(gm, v)).make_preferred()).c_str(), true);
    // the session's frame rate: the game writes this file (the God of War: Ascension reads its own variable's name)
    g.perf_file = (g.hub_dir / "desempenho" / "partida.json").make_preferred();
    fs::create_directories(g.perf_file.parent_path(), ec);
    fs::remove(g.perf_file, ec);
    SDL_SetEnvironmentVariable(env, "OTAVIX_DESEMPENHO", U8(g.perf_file).c_str(), true);
    SDL_SetEnvironmentVariable(env, "GOWA_PERF_JSON", U8(g.perf_file).c_str(), true);
    // the on-screen performance meter, off unless the player turned it on (F10 shows or hides it in the game)
    if (g.state.Bool("osd", false)) {
        SDL_SetEnvironmentVariable(env, "OTAVIX_OSD", "1", true);
        SDL_SetEnvironmentVariable(env, "GOWA_OSD", "1", true);
        SDL_SetEnvironmentVariable(env, "SH_OSD", "1", true);  // Silent Hill (PSXRecomp): its sh1.medidor mod
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    std::string wd = U8(gm.prof.exe_workdir       ? exe_path.parent_path()
                        : !gm.prof.workdir.empty() ? (gm.entry.dir / FromU8(gm.prof.workdir)).make_preferred()
                                                   : gm.entry.dir);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, wd.c_str());
    // a port that writes its log to the console: it goes to log_file
    if (!gm.prof.log_file.empty()) {
        fs::path log = gm.entry.dir / FromU8(gm.prof.log_file);
        fs::create_directories(log.parent_path(), ec);
        if ((g.proc_log = SDL_IOFromFile(U8(log).c_str(), "wb"))) {
            SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
            SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, g.proc_log);
            SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        }
    }
    g.music.Stop();
    g.proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(env);
    if (!g.proc) {
        Toast(std::string(Tr(S::StartFailed)) + ": " + SDL_GetError());
        if (g.proc_log) SDL_CloseIO(g.proc_log);
        g.proc_log = nullptr;
        return;
    }
    g.proc_game = index;
    g.started = SDL_GetTicks();
    g.state.Set("last_game", gm.entry.id);
    std::string on_play = g.state.Str("on_play", "minimize");
    if (on_play == "close") {
        // the game keeps running on its own
        SDL_DestroyProcess(g.proc);
        g.proc = nullptr;
        g.quit = true;
    } else if (on_play == "minimize") {
        SDL_MinimizeWindow(g.win);
    }
    // the controller now plays the game: the launcher's menus stop reading it (only Back + Start is watched)
    if (g.proc) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
}

void OpenGame(int i);
std::string IssueUrl(Game& gm, const std::string& kind, const std::string& what);
void OpenIssue(Game& gm, const std::string& kind, const std::string& what);
const std::string& Server();
Headers ServerHeaders(bool admin = false);
const std::string& Str(const std::vector<std::string>& row, size_t i);

// ---------------------------------------------------------------------------------------------- performance

fs::path PerfHistory() { return g.hub_dir / "desempenho" / "historico.tsv"; }

// the options that matter most to the frame rate, those a profile has of these, in this order
const char* kSummaryKeys[] = {"fps", "resolution_scale", "escala", "resolucao", "render_scale", "upscaler", "filtro"};

// a game's options as they are now, the ones a session ran with: all but paths and button maps
std::string SessionOptions(Game& gm, std::string& summary) {
    std::vector<std::pair<std::string, std::string>> v;
    for (const Option& o : gm.prof.options) {
        if (o.type == OptType::Path || o.type == OptType::ButtonMap) continue;
        v.push_back({o.key, ToText(gm.store.Get(o))});
    }
    summary.clear();
    for (const char* key : kSummaryKeys)
        for (auto& [k, val] : v)
            if (k == key) summary += (summary.empty() ? "" : ";") + k + "=" + val;
    return OptionsJson(v);
}

// a summary ("fps=60;resolution_scale=2") as the game's own labels: "60 fps · 2x (1440p)"
std::string SummaryText(Game& gm, const std::string& summary) {
    std::string out;
    size_t a = 0;
    while (a < summary.size()) {
        size_t b = summary.find(';', a);
        if (b == std::string::npos) b = summary.size();
        std::string item = summary.substr(a, b - a), key = item.substr(0, item.find('=')), val;
        if (item.find('=') != std::string::npos) val = item.substr(item.find('=') + 1);
        std::string shown = val;
        if (const Option* o = gm.prof.Find(key))
            for (const ChoiceItem& c : o->choices)
                if (ToText(c.value) == val) shown = Tx(c.label);
        out += (out.empty() ? "" : " · ") + shown;
        a = b + 1;
    }
    return out;
}

void SendSession(const Session& s) {
    if (Server().empty()) return;
    g.perf_call.Start("POST", Server() + "/v1/desempenho", ServerHeaders(), ReportJson(ThisMachine(), s, g.install_id));
}

// a game closed: the frame rate it measured, kept on this computer and, if the player agreed, sent
void RecordSession(Game& gm) {
    Session s;
    s.when = int64_t(SDL_GetTicks() / 1000);
    SDL_Time now = 0;
    if (SDL_GetCurrentTime(&now)) s.when = int64_t(now / SDL_NS_PER_SECOND);
    s.game = gm.entry.id;
    s.port = InstalledPortVersion(gm.entry.dir);
    s.minutes = gm.last_minutes;
    s.options = SessionOptions(gm, s.summary);
    std::error_code ec;
    if (g.perf_file.empty() || !ReadGameFile(g.perf_file, s)) return;
    fs::remove(g.perf_file, ec);
    AppendHistory(PerfHistory(), s);
    std::string share = g.state.Str("perf_share", "");
    if (share == "sim") {
        SendSession(s);
    } else if (share.empty()) {
        g.perf_pending = s;
        g.perf_ask = true;
    }
}

// a report as the player reads it: a key a line
std::string PrettyJson(const std::string& json) {
    std::string out;
    int depth = 0;
    bool in_string = false;
    auto newline = [&] { out += "\n" + std::string(size_t(depth) * 2, ' '); };
    for (size_t i = 0; i < json.size(); i++) {
        char c = json[i];
        if (in_string) {
            out += c;
            if (c == '\\' && i + 1 < json.size()) out += json[++i];
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out += c;
        } else if (c == '{') {
            out += c;
            depth++;
            if (i + 1 < json.size() && json[i + 1] != '}') newline();
        } else if (c == '}') {
            if (i > 0 && json[i - 1] != '{') {
                depth--;
                newline();
            } else {
                depth--;
            }
            out += c;
        } else if (c == ',') {
            out += c;
            newline();
        } else if (c == ':') {
            out += ": ";
        } else {
            out += c;
        }
    }
    return out;
}

void PollGame() {
    if (!g.proc) return;
    int code = 0;
    if (!SDL_WaitProcess(g.proc, false, &code)) return;
    SDL_DestroyProcess(g.proc);
    g.proc = nullptr;
    if (g.proc_log) SDL_CloseIO(g.proc_log);
    g.proc_log = nullptr;
    if (g.proc_game >= 0) {
        Game& gm = *g.games[size_t(g.proc_game)];
        gm.last_exit = code;
        gm.last_minutes = double(SDL_GetTicks() - g.started) / 60000.0;
        gm.backups_stale = true;
        RecordSession(gm);
        gm.icon.Release();
        // closed with an error: its page shows it, with the report ready
        if (code != 0 && !g.closing) {
            gm.crashed = true;
            if (g.cur != g.proc_game || g.screen != Screen::Game) OpenGame(g.proc_game);
            // tests: LAUNCHER_ISSUE_FILE=path gets the report's address as soon as the game fails
            if (const char* f = SDL_getenv("LAUNCHER_ISSUE_FILE")) std::ofstream(f) << IssueUrl(gm, "Crash", "teste");
        }
    }
    g.closing = false;
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoFirst);
    SDL_RestoreWindow(g.win);
    SDL_RaiseWindow(g.win);
    PlayTheme();
}

// Back (Select) and Start held together on any controller
bool CloseComboHeld() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    bool held = false;
    for (int i = 0; i < n && !held; i++) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) pad = SDL_OpenGamepad(ids[i]);
        held = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    }
    SDL_free(ids);
    return held;
}

// while a game runs: Back + Start for 2 s asks it to close, as its window's X would; after 5 s more it is ended
void WatchCloseCombo() {
    static Uint64 since = 0, asked = 0;
    if (!g.proc) {
        since = asked = 0;
        return;
    }
    uint64_t pid = uint64_t(SDL_GetNumberProperty(SDL_GetProcessProperties(g.proc), SDL_PROP_PROCESS_PID_NUMBER, 0));
    Uint64 now = SDL_GetTicks();
    if (asked) {
        if (now - asked > 5000 && now - asked < 6000) EndProcessTree(pid);
        return;
    }
    // tests: LAUNCHER_CLOSE_AFTER=seconds holds Back + Start from that long after the game started
    static const char* test = SDL_getenv("LAUNCHER_CLOSE_AFTER");
    bool held = test ? now - g.started > Uint64(std::atoi(test)) * 1000 : g.state.Bool("close_combo", true) && CloseComboHeld();
    if (!held) {
        since = 0;
        return;
    }
    if (!since) since = now;
    if (now - since < 2000) return;
    g.closing = true;
    asked = now;
    if (!AskToClose(pid)) EndProcessTree(pid);
}

// while a game runs: its windows get its icon (title bar, taskbar, Alt+Tab)
void WatchIcon() {
    if (!g.proc || g.proc_game < 0) return;
    uint64_t pid = uint64_t(SDL_GetNumberProperty(SDL_GetProcessProperties(g.proc), SDL_PROP_PROCESS_PID_NUMBER, 0));
    g.games[size_t(g.proc_game)]->icon.Apply(pid);
}

// ---------------------------------------------------------------------------------------------- option widgets

void ModesOfPrimary() {
    g.modes.clear();
    int n = 0;
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &n);
    std::set<std::pair<int, int>> seen;
    for (int i = 0; i < n; i++) {
        int w = int(std::lround(modes[i]->w * modes[i]->pixel_density));
        int h = int(std::lround(modes[i]->h * modes[i]->pixel_density));
        if (seen.insert({w, h}).second) g.modes.push_back({w, h});
    }
    SDL_free(modes);
    std::sort(g.modes.begin(), g.modes.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
}

int DesktopHeight() {
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    return m ? int(std::lround(m->h * m->pixel_density)) : 1080;
}

// a row's label on the left, its control from the middle on
void RowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f + ImGui::GetStyle().WindowPadding.x);
    ImGui::SetNextItemWidth(-1);
}

void NoteHelp(const Text& help) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) g.help = help.Get(g.lang);
}

// a settings key as the game sees it: stored, else the game's default
Value KeyValue(const std::string& key, const Value& like) {
    Game& gm = G();
    const Value* d = gm.prof.DefaultFor(key);
    if (const Option* o = gm.prof.Find(key)) return gm.store.Get(*o);
    return gm.store.GetKey(key, d ? *d : like);
}

bool ItemChosen(const ChoiceItem& c, const Value& cur) {
    if (c.set.empty()) return SameValue(c.value, cur);
    for (auto& [k, v] : c.set)
        if (!SameValue(KeyValue(k, v), v)) return false;
    return true;
}

void SetKey(const std::string& k, const Value& v) {
    if (const Option* o = G().prof.Find(k)) G().store.Set(*o, v);
    else G().store.SetKey(k, v);
}

void Choose(const Option& o, const ChoiceItem& c) {
    if (c.set.empty()) {
        G().store.Set(o, c.value);
        return;
    }
    for (auto& [k, v] : c.set) SetKey(k, v);
}

void ChoiceWidget(const Option& o) {
    Value cur = G().store.Get(o);
    const ChoiceItem* chosen = nullptr;
    for (const ChoiceItem& c : o.choices)
        if (ItemChosen(c, cur)) {
            chosen = &c;
            break;
        }
    std::string label = chosen ? chosen->label.Get(g.lang) : ToText(cur);
    if (ImGui::BeginCombo("##v", label.c_str())) {
        for (const ChoiceItem& c : o.choices) {
            bool sel = &c == chosen;
            if (ImGui::Selectable(c.label.Get(g.lang).c_str(), sel)) Choose(o, c);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void ToggleWidget(const Option& o) {
    Value cur = G().store.Get(o);
    bool b = std::holds_alternative<bool>(cur) ? std::get<bool>(cur) : ToText(cur) != "0";
    if (ImGui::Checkbox("##v", &b)) G().store.Set(o, b);
}

void SliderWidget(const Option& o) {
    Value cur = G().store.Get(o);
    int v = int(std::holds_alternative<int64_t>(cur) ? std::get<int64_t>(cur) : std::atoll(ToText(cur).c_str()));
    // the suffix goes into a format: its % doubled
    std::string suffix;
    for (char ch : o.suffix) suffix += ch == '%' ? std::string("%%") : std::string(1, ch);
    std::string fmt = "%d" + suffix;
    if (o.guest_height) {
        // the picture's height inside the game: the output's height times the percent
        int out_h = int(G().store.GetInt("height", 0));
        if (!out_h) out_h = DesktopHeight();
        int inner = out_h * v / 100;
        char b[96];
        std::snprintf(b, sizeof(b), "  (%dp %s)", inner, Tr(S::Internal));
        fmt += b;
    }
    // no typing in: a controller cannot, and a focus request would open the text field
    if (ImGui::SliderInt("##v", &v, int(o.min), int(o.max), fmt.c_str(),
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
        if (o.step > 1) v = int(o.min + (v - o.min + o.step / 2) / o.step * o.step);
        G().store.Set(o, int64_t(v));
    }
}

// window and screen sizes: the monitor's modes and the usual sizes of each shape, up to the largest mode
struct SizeGroup {
    const char* name;
    std::vector<std::pair<int, int>> sizes;
};

std::vector<SizeGroup> SizeGroups() {
    static const std::vector<SizeGroup> kUsual = {
        {"16:9", {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}}},
        {"16:10", {{1280, 800}, {1440, 900}, {1680, 1050}, {1920, 1200}, {2560, 1600}, {3840, 2400}}},
        {"4:3", {{1024, 768}, {1280, 960}, {1600, 1200}, {2048, 1536}}},
        {"21:9", {{2560, 1080}, {3440, 1440}, {5120, 2160}}},
    };
    int max_w = 0, max_h = 0;
    for (auto [w, h] : g.modes) {
        max_w = std::max(max_w, w);
        max_h = std::max(max_h, h);
    }
    if (!max_w) max_w = 7680, max_h = 4320;
    std::vector<SizeGroup> out;
    std::set<std::pair<int, int>> seen;
    for (const SizeGroup& grp : kUsual) {
        SizeGroup kept{grp.name, {}};
        for (auto sz : grp.sizes)
            if (sz.first <= max_w && sz.second <= max_h && seen.insert(sz).second) kept.sizes.push_back(sz);
        if (!kept.sizes.empty()) out.push_back(std::move(kept));
    }
    // the monitor's own modes not in the usual lists
    SizeGroup monitor{nullptr, {}};
    for (auto sz : g.modes)
        if (seen.insert(sz).second) monitor.sizes.push_back(sz);
    if (!monitor.sizes.empty()) out.push_back(std::move(monitor));
    return out;
}

void ResolutionWidget(const Option& o) {
    Store& st = G().store;
    int w = int(st.GetInt(o.width_key, 0)), h = int(st.GetInt(o.height_key, 0));
    char cur[64];
    if (!w || !h) std::snprintf(cur, sizeof(cur), "%s", Tr(S::Desktop));
    else std::snprintf(cur, sizeof(cur), "%d x %d", w, h);
    if (ImGui::BeginCombo("##v", cur, ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(Tr(S::Desktop), !w || !h)) {
            st.SetInt(o.width_key, 0);
            st.SetInt(o.height_key, 0);
        }
        for (const SizeGroup& grp : SizeGroups()) {
            ImGui::SeparatorText(grp.name ? grp.name : Tr(S::MonitorModes));
            for (auto [mw, mh] : grp.sizes) {
                char b[32];
                std::snprintf(b, sizeof(b), "%d x %d", mw, mh);
                bool sel = mw == w && mh == h;
                if (ImGui::Selectable(b, sel)) {
                    st.SetInt(o.width_key, mw);
                    st.SetInt(o.height_key, mh);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void PathWidget(const Option& o) {
    Value cur = G().store.Get(o);
    std::string p = ToText(cur);
    ImGui::BeginDisabled();
    std::string shown = p.empty() ? std::string("(") + Tr(S::Default) + ")" : p;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##p", shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    float x = ImGui::GetContentRegionAvail().x * 0.45f + ImGui::GetStyle().WindowPadding.x;
    ImGui::SetCursorPosX(x);
    if (o.pick != "file" && ImGui::Button(Tr(S::PickFolder))) PickPath(o, true);
    NoteHelp(o.help);
    if (o.pick != "folder") {
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::PickFile))) PickPath(o, false);
        NoteHelp(o.help);
    }
    if (!p.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::Clear))) G().store.Set(o, std::string());
        NoteHelp(o.help);
    }
}

std::string PadName(const std::string& sdl) {
    // the names a player sees, Xbox style; SDL's otherwise
    static const std::pair<const char*, const char*> kNames[] = {
        {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"leftshoulder", "LB"}, {"rightshoulder", "RB"},
        {"lefttrigger", "LT"}, {"righttrigger", "RT"}, {"start", "Start"}, {"back", "Back / View"},
        {"leftstick", "L3"}, {"rightstick", "R3"}, {"dpup", "D-pad ↑"}, {"dpdown", "D-pad ↓"},
        {"dpleft", "D-pad ←"}, {"dpright", "D-pad →"}, {"guide", "Guide"}};
    for (auto [k, v] : kNames)
        if (sdl == k) return v;
    return sdl;
}

void ButtonMapWidget(const Option& o) {
    ImGui::TextUnformatted(Tx(o.label));
    if (!o.help.Empty()) Dim(Tx(o.help));
    if (ImGui::BeginTable("map", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const ButtonSlot& b : o.buttons) {
            ImGui::PushID(b.key.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Tx(b.label));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            bool waiting = g.cap_slot == &b;
            if (waiting) ImGui::TextColored(Accent(1.3f), "%s", Tr(S::ButtonWaiting));
            else ImGui::TextUnformatted(PadName(G().store.GetButton(o, b)).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Button(waiting ? Tr(S::ButtonCancel) : Tr(S::Change))) {
                if (waiting) {
                    g.cap_opt = nullptr;
                    g.cap_slot = nullptr;
                } else {
                    g.cap_opt = &o;
                    g.cap_slot = &b;
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(Tr(S::ResetButtons)))
        for (const ButtonSlot& b : o.buttons) G().store.SetButton(o, b, b.def);
}

void OptionRow(const Option& o) {
    ImGui::PushID(o.key.c_str());
    switch (o.type) {
        case OptType::Choice: RowLabel(Tx(o.label)); ChoiceWidget(o); NoteHelp(o.help); break;
        case OptType::Toggle: RowLabel(Tx(o.label)); ToggleWidget(o); NoteHelp(o.help); break;
        case OptType::Slider: RowLabel(Tx(o.label)); SliderWidget(o); NoteHelp(o.help); break;
        case OptType::Resolution: RowLabel(Tx(o.label)); ResolutionWidget(o); NoteHelp(o.help); break;
        case OptType::Path: RowLabel(Tx(o.label)); PathWidget(o); break;
        case OptType::ButtonMap: ButtonMapWidget(o); break;
    }
    ImGui::PopID();
}

// a pad button pressed while a ButtonMap entry waits for one: it takes it, nothing else sees it
bool CaptureEvent(const SDL_Event& e) {
    if (!g.cap_slot) return false;
    std::string name;
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const char* n = SDL_GetGamepadStringForButton(SDL_GamepadButton(e.gbutton.button));
        if (n) name = n;
    } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 24000 &&
               (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        name = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(e.gaxis.axis));
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        g.cap_slot = nullptr;
        g.cap_opt = nullptr;
        return true;
    } else {
        return e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    }
    if (!name.empty() && g.cur >= 0) {
        G().store.SetButton(*g.cap_opt, *g.cap_slot, name);
        g.cap_slot = nullptr;
        g.cap_opt = nullptr;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- the art behind

// the game's icon (its window's and its shortcut's), in the background: from the player's game, else the web
void StartIcon(Game& gm) {
    if (gm.icon_spec.Empty()) return;
    std::vector<std::string> files;
    for (const std::string& f : gm.icon_spec.files) {
        // a path in an option the player has not filled in yet is left out
        size_t a = f.find('{'), b = f.find('}');
        if (a != std::string::npos && b != std::string::npos && Expand(gm, f.substr(a, b - a + 1)).empty()) continue;
        files.push_back(Expand(gm, f));
    }
    fs::path id = FromU8(gm.entry.id);
    gm.icon.Load(gm.icon_spec, FindGame(gm), files, g.hub_dir / "cache" / "arte" / id, g.hub_dir / "dados" / id / "icone.ico");
}

void StartArt(Game& gm) {
    StartIcon(gm);
    if (gm.art_spec.Empty()) return;
    fs::path game = gm.art_spec.folder.empty() ? FindGame(gm) : gm.art_spec.folder;
    if (game.empty() && gm.art_spec.web.empty()) return;
    fs::path cache = g.hub_dir / "cache" / "arte" / FromU8(gm.entry.id);
    std::thread([&gm, spec = gm.art_spec, game, cache] {
        std::vector<ArtImage> imgs;
        if (!game.empty()) imgs = LoadArt(spec, game);
        // nothing from the player's copy yet: the public pictures
        if (imgs.empty() && !spec.web.empty()) {
            imgs = LoadWebArt(spec, cache);
            // one picture (a box): the card's and the page's
            if (!imgs.empty() && int(imgs.size()) <= spec.card_only) imgs.push_back(imgs.back());
        }
        std::lock_guard lock(gm.art_m);
        gm.art_loaded = std::move(imgs);
        gm.art_ready = true;
    }).detach();
}

void TakeArt() {
    for (auto& gp : g.games) {
        Game& gm = *gp;
        std::lock_guard lock(gm.art_m);
        if (!gm.art_ready) continue;
        gm.art_ready = false;
        for (auto& [t, s] : gm.art) SDL_DestroyTexture(t);
        gm.art.clear();
        for (ArtImage& img : gm.art_loaded) {
            SDL_Texture* t = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.w, img.h);
            if (!t) continue;
            SDL_UpdateTexture(t, nullptr, img.rgba.data(), img.w * 4);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            gm.art.push_back({t, ImVec2(float(img.w), float(img.h))});
        }
        gm.art_loaded.clear();
    }
}

ImTextureRef Tex(SDL_Texture* t) { return ImTextureRef(ImTextureID(intptr_t(t))); }

// the launcher's symbol (launcher/icone.png), loaded once; null without the file
SDL_Texture* BrandIcon() {
    static SDL_Texture* icon = nullptr;
    static bool tried = false;
    if (tried) return icon;
    tried = true;
    ArtImage img;
    if (!ReadImageFile(g.hub_dir / "launcher" / "icone.png", img)) return nullptr;
    icon = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.w, img.h);
    if (!icon) return nullptr;
    SDL_UpdateTexture(icon, nullptr, img.rgba.data(), img.w * 4);
    SDL_SetTextureScaleMode(icon, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(icon, SDL_BLENDMODE_BLEND);
    return icon;
}

// the uv window of a picture that covers a box of this aspect, centred
void CoverUv(ImVec2 ts, ImVec2 box, ImVec2& uv0, ImVec2& uv1) {
    float a = ts.x / ts.y, b = box.x / box.y;
    if (a > b) {
        float w = b / a;
        uv0 = ImVec2((1 - w) / 2, 0);
        uv1 = ImVec2((1 + w) / 2, 1);
    } else {
        float h = a / b;
        uv0 = ImVec2(0, (1 - h) / 2);
        uv1 = ImVec2(1, (1 + h) / 2);
    }
}

// the pictures in turn, each slowly drawing nearer, crossfading into the next; darkened where the words are
void DrawArt(ImDrawList* dl, ImVec2 size, const Art& art, float seconds) {
    dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(8, 8, 9, 255));
    if (art.empty()) return;
    {
        bool anim = g.state.Bool("bg_anim", true);
        double secs = std::max(4.0f, seconds), fade = 1.6;
        double t = anim ? double(SDL_GetTicks() - g.art_t0) / 1000.0 : 0.0;
        size_t n = art.size(), i = size_t(t / secs) % n;
        double in = std::fmod(t, secs);
        auto draw = [&](size_t k, double local, float alpha) {
            auto [tex, ts] = art[k];
            // cover the window, zooming from 1.0 to 1.08 over the picture's time
            float zoom = 1.0f + (anim ? 0.08f * float(std::clamp(local / (secs + fade), 0.0, 1.0)) : 0.0f);
            float s = std::max(size.x / ts.x, size.y / ts.y) * zoom;
            ImVec2 d(ts.x * s, ts.y * s), o((size.x - d.x) * 0.5f, (size.y - d.y) * 0.5f);
            dl->AddImage(Tex(tex), o, ImVec2(o.x + d.x, o.y + d.y), ImVec2(0, 0), ImVec2(1, 1),
                         IM_COL32(255, 255, 255, int(alpha * 255)));
        };
        draw(i, in, 1.0f);
        if (anim && in > secs - fade && n > 1) draw((i + 1) % n, in - secs, float((in - (secs - fade)) / fade));
    }
    // the sidebar and the words stay readable: dark from the left and the bottom, the colour of the page faint
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(8, 8, 10, 235), IM_COL32(8, 8, 10, 120),
                                IM_COL32(8, 8, 10, 170), IM_COL32(8, 8, 10, 245));
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, size.y * 0.7f), ImGui::GetColorU32(Accent(0.5f, 0.22f)),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
}

// a game's page background: its pictures but the card's
Art PageArt(const Game& gm) {
    size_t skip = size_t(std::max(0, gm.art_spec.card_only));
    if (gm.art.size() <= skip) return gm.art;
    return Art(gm.art.begin() + std::ptrdiff_t(skip), gm.art.end());
}

// ---------------------------------------------------------------------------------------------- a game's pages

// the game's sidebar: home, its own pages, the advanced options (its advanced pages as tabs), saves, about
enum class Kind { Home, Page, Advanced, Saves, Mods, Perf, News, Report, About };
struct Entry {
    Kind kind;
    int page = -1;  // Kind::Page: index in prof.pages
};

std::vector<Entry> Entries() {
    std::vector<Entry> e{{Kind::Home}};
    // a version that does not play here: its state, what the port's updates bring (a port still in development) and
    // the credits
    if (!Playable(G(), g.view)) {
        if (!G().entry.repo.empty()) e.push_back({Kind::News});
        e.push_back({Kind::About});
        return e;
    }
    bool advanced = false;
    for (size_t i = 0; i < G().prof.pages.size(); i++) {
        if (G().prof.pages[i].advanced) advanced = true;
        else e.push_back({Kind::Page, int(i)});
    }
    if (advanced) e.push_back({Kind::Advanced});
    e.push_back({Kind::Saves});
    if (!G().prof.mods_folder.empty()) e.push_back({Kind::Mods});
    e.push_back({Kind::Perf});
    if (!G().entry.repo.empty()) e.push_back({Kind::News});   // a port: what each update changed
    e.push_back({Kind::Report});
    e.push_back({Kind::About});
    return e;
}

std::vector<int> AdvancedPages() {
    std::vector<int> v;
    for (size_t i = 0; i < G().prof.pages.size(); i++)
        if (G().prof.pages[i].advanced) v.push_back(int(i));
    return v;
}

Entry Current() {
    auto e = Entries();
    return e[size_t(std::clamp(G().page, 0, int(e.size()) - 1))];
}

const char* PageName(const Entry& e) {
    switch (e.kind) {
        case Kind::Home: return Tr(S::Home);
        case Kind::Saves: return Tr(S::Saves);
        case Kind::Mods: return Tr(S::Mods);
        case Kind::Perf: return Tr(S::Perf);
        case Kind::News: return Tr(S::News);
        case Kind::Report: return Tr(S::Report);
        case Kind::About: return Tr(S::About);
        case Kind::Advanced: return Tr(S::Advanced);
        case Kind::Page: return Tx(G().prof.pages[size_t(e.page)].label);
    }
    return "";
}

// ---------------------------------------------------------------------------------------------- installing a game

void InstallPicked(void*, const char* const* files, int) {
    std::lock_guard lock(g.dlg_m);
    if (files && files[0]) {
        g.inst_path = files[0];
        g.inst_ready = true;
    }
}

// what the player downloaded: an archive or a disc image, or the folder of a game already unpacked
enum class Pick { Download, Image, Folder, File };

void PickInstall(Pick what) {
    {
        std::lock_guard lock(g.dlg_m);
        g.inst_game = g.cur;
        g.inst_ready = false;
    }
    std::string start;
    if (const char* home = SDL_getenv("USERPROFILE")) start = std::string(home) + "\\Downloads";
    if (what == Pick::Folder) {
        SDL_ShowOpenFolderDialog(InstallPicked, nullptr, g.win, start.c_str(), false);
    } else if (what == Pick::File) {
        // the profile's extensions: "Jogo (.jar)", "jar"
        static std::string name, pattern;
        name = "Jogo (";
        pattern.clear();
        for (const std::string& e : G().prof.install.files) {
            name += (pattern.empty() ? "." : ", .") + e;
            pattern += (pattern.empty() ? "" : ";") + e;
        }
        name += ")";
        static SDL_DialogFileFilter filter;
        filter = {name.c_str(), pattern.c_str()};
        SDL_ShowOpenFileDialog(InstallPicked, nullptr, g.win, &filter, 1, start.c_str(), false);
    } else if (what == Pick::Image) {
        static const SDL_DialogFileFilter kImage[] = {{"Imagem do disco (.iso)", "iso;xiso"}};
        SDL_ShowOpenFileDialog(InstallPicked, nullptr, g.win, kImage, 1, start.c_str(), false);
    } else {
        static const SDL_DialogFileFilter kGame[] = {{"Jogo baixado (.7z, .zip, .rar, .iso)", "7z;zip;rar;iso;xiso"}};
        SDL_ShowOpenFileDialog(InstallPicked, nullptr, g.win, kGame, 1, start.c_str(), false);
    }
}

void TakeInstallPick() {
    std::lock_guard lock(g.dlg_m);
    if (!g.inst_ready || g.inst_game != g.cur) return;
    g.inst_ready = false;
    Game& gm = G();
    gm.install_taken = false;
    gm.confirm_delete_archive = false;
    std::string folder = gm.prof.install.folder.empty() ? "jogo" : gm.prof.install.folder;
    gm.installer.Start(FromU8(g.inst_path), gm.entry.dir / FromU8(folder), gm.prof.install);
}

void MakeShortcut(Game& gm) {
    std::error_code ec;
    // the game's own icon, else its executable's
    fs::path icon = gm.icon.Ico();
    if (icon.empty() || !fs::is_regular_file(icon, ec)) icon = gm.entry.dir / FromU8(gm.prof.exe);
    if (!fs::is_regular_file(icon, ec)) icon.clear();
    std::string err;
    if (DesktopShortcut(Tx(gm.prof.title), "--jogo " + gm.entry.id, icon, g.hub_dir, err)) Toast(Tr(S::ShortcutDone));
    else Toast(std::string(Tr(S::ShortcutFailed)) + ": " + err);
}

// ---------------------------------------------------------------------------------------------- how to use

struct TutorialStep {
    Text title, text;
};

Text T3(const char* pt, const char* en, const char* es) {
    Text t;
    t.pt = pt, t.en = en, t.es = es;
    return t;
}

void TutorialPage() {
    static const std::vector<TutorialStep> kSteps = {
        {T3("Tenha o seu jogo", "Have your game", "Ten tu juego"),
         T3("Baixe a sua cópia do jogo. Pode ser o arquivo compactado (.7z, .zip ou .rar) ou a imagem do disco (.iso). "
            "Não precisa descompactar nada: o launcher faz isso. A página de cada jogo diz qual versão ele precisa.",
            "Download your copy of the game. It can be the archive (.7z, .zip or .rar) or the disc image (.iso). No need to "
            "unpack anything: the launcher does it. Each game's page says which release it needs.",
            "Descarga tu copia del juego. Puede ser el archivo comprimido (.7z, .zip o .rar) o la imagen del disco (.iso). "
            "No hace falta descomprimir nada: el launcher lo hace. La página de cada juego dice qué versión necesita.")},
        {T3("Abra o jogo aqui", "Open the game here", "Abre el juego aquí"),
         T3("Na biblioteca, clique no jogo (ou selecione e aperte A no controle).",
            "In the library, click the game (or select it and press A on the controller).",
            "En la biblioteca, haz clic en el juego (o selecciónalo y pulsa A en el mando).")},
        {T3("Instale o jogo", "Install the game", "Instala el juego"),
         T3("Clique em \"Instalação automática\" e escolha o arquivo que você baixou (normalmente está em Downloads): o "
            "launcher descompacta, confere se é a versão certa e deixa tudo pronto, e no fim oferece apagar o download para "
            "liberar espaço. Prefere fazer à mão? Use \"Instalação manual\": você descompacta e só mostra onde está o jogo.",
            "Click \"Automatic install\" and pick the file you downloaded (usually in Downloads): the launcher unpacks it, "
            "checks it is the right release and sets it all up, then offers to delete the download to free space. Rather do it "
            "by hand? Use \"Manual install\": you unpack it and just show where the game is.",
            "Haz clic en \"Instalación automática\" y elige el archivo que descargaste (normalmente en Descargas): el "
            "launcher lo descomprime, comprueba que sea la versión correcta y deja todo listo, y al final ofrece borrar la "
            "descarga. ¿Prefieres hacerlo a mano? Usa \"Instalación manual\": lo descomprimes y solo muestras dónde está.")},
        {T3("Clique em JOGAR", "Click PLAY", "Haz clic en JUGAR"),
         T3("\"Equilibrado\" é o recomendado. Na primeira vez o jogo se prepara para o seu PC por alguns minutos; "
            "depois abre direto.",
            "\"Balanced\" is the recommended one. The first time, the game prepares itself for your PC for a few minutes; "
            "after that it starts right away.",
            "\"Equilibrado\" es el recomendado. La primera vez el juego se prepara para tu PC durante unos minutos; "
            "después abre directo.")},
    };
    static const std::vector<Text> kTips = {
        T3("Controle: A seleciona, B volta, LB e RB trocam as páginas, LT e RT trocam as abas.",
           "Controller: A selects, B goes back, LB and RB switch pages, LT and RT switch tabs.",
           "Mando: A selecciona, B vuelve, LB y RB cambian de página, LT y RT cambian de pestaña."),
        T3("Para fechar o jogo pelo controle, segure Back + Start (Select + Start no PlayStation) por 2 segundos. No teclado, Alt+F4.",
           "To close the game from the controller, hold Back + Start (Select + Start on PlayStation) for 2 seconds. On the keyboard, Alt+F4.",
           "Para cerrar el juego con el mando, mantén Back + Start (Select + Start en PlayStation) 2 segundos. En el teclado, Alt+F4."),
        T3("Na página do jogo dá para criar um atalho na área de trabalho que abre direto nele.",
           "On the game's page you can make a desktop shortcut that opens straight into it.",
           "En la página del juego puedes crear un acceso directo en el escritorio que abre directo en él."),
        T3("Os saves ganham cópia de segurança automática antes de cada partida (página Saves).",
           "Saves get an automatic backup before each session (Saves page).",
           "Las partidas guardadas reciben copia de seguridad automática antes de cada sesión (página Saves)."),
        T3("Deu problema? Use \"Reportar problema\" na página do jogo: ele junta as informações técnicas por você.",
           "Something wrong? Use \"Report a problem\" on the game's page: it gathers the technical details for you.",
           "¿Algún problema? Usa \"Reportar un problema\" en la página del juego: reúne los datos técnicos por ti."),
    };
    static const std::vector<std::pair<Text, Text>> kFaq = {
        {T3("Prefiro instalar à mão.", "I would rather install by hand.", "Prefiero instalar a mano."),
         T3("Descompacte o arquivo baixado (botão direito > Extrair tudo, ou 7-Zip/WinRAR). Na página do jogo, abra "
            "\"Instalação manual\" e escolha a .iso que saiu (ou a pasta do jogo). O launcher confere a versão e usa o jogo "
            "de onde ele está: não apague esse arquivo depois.",
            "Unpack the download (right click > Extract all, or 7-Zip/WinRAR). On the game's page, open \"Manual install\" and "
            "pick the .iso that came out (or the game's folder). The launcher checks the release and uses the game where it is: "
            "do not delete that file afterwards.",
            "Descomprime la descarga (clic derecho > Extraer todo, o 7-Zip/WinRAR). En la página del juego, abre \"Instalación "
            "manual\" y elige la .iso que salió (o la carpeta del juego). El launcher comprueba la versión y usa el juego donde "
            "está: no borres ese archivo después.")},
        {T3("O launcher disse que é outra versão do jogo.", "The launcher says it is another release of the game.",
            "El launcher dice que es otra versión del juego."),
         T3("O port foi feito para uma versão específica (ela aparece na página do jogo). Baixe essa versão e instale de novo.",
            "The port was made for one release (shown on the game's page). Download that one and install again.",
            "El port se hizo para una versión concreta (aparece en la página del juego). Descarga esa y vuelve a instalar.")},
        {T3("Faltou espaço no disco.", "Not enough disk space.", "Falta espacio en el disco."),
         T3("O jogo descompactado ocupa mais que o arquivo baixado. Libere espaço no disco onde o launcher está e tente de novo.",
            "The unpacked game takes more than the download. Free some space on the launcher's drive and try again.",
            "El juego descomprimido ocupa más que la descarga. Libera espacio en el disco del launcher e inténtalo de nuevo.")},
        {T3("Já tenho o jogo descompactado numa pasta.", "I already have the game unpacked in a folder.",
            "Ya tengo el juego descomprimido en una carpeta."),
         T3("Use \"Já tenho a pasta do jogo\" e escolha essa pasta: o launcher confere e usa de lá mesmo.",
            "Use \"I have the game's folder\" and pick it: the launcher checks it and uses it from there.",
            "Usa \"Ya tengo la carpeta del juego\" y elígela: el launcher la comprueba y la usa desde ahí.")},
    };

    Heading(Tr(S::Tutorial));
    Dim(Tr(S::TutorialIntro));
    ImGui::Spacing();
    float pad = 16 * g.scale, r = 22 * g.scale;
    for (size_t i = 0; i < kSteps.size(); i++) {
        ImGui::PushID(int(i));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.13f, 0.92f));
        ImGui::BeginChild("step", ImVec2(-1, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        // the step's number in the accent's circle
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(at.x + r, at.y + r), r, ImGui::GetColorU32(Accent(0.85f)));
        char n[4];
        std::snprintf(n, sizeof(n), "%zu", i + 1);
        ImGui::PushFont(g.bold, 24);
        ImVec2 ts = ImGui::CalcTextSize(n);
        dl->AddText(ImVec2(at.x + r - ts.x / 2, at.y + r - ts.y / 2), IM_COL32_WHITE, n);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(r * 2, r * 2));
        ImGui::SameLine(0, pad);
        ImGui::BeginGroup();
        ImGui::PushFont(g.bold, 22);
        ImGui::TextUnformatted(Tx(kSteps[i].title));
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.84f, 1));
        ImGui::TextUnformatted(Tx(kSteps[i].text));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::Dummy(ImVec2(0, 8 * g.scale));
    ImGui::PushFont(g.bold, 22);
    ImGui::TextUnformatted(Tr(S::Tips));
    ImGui::PopFont();
    for (const Text& t : kTips) {
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", Tx(t));
    }
    ImGui::Dummy(ImVec2(0, 8 * g.scale));
    ImGui::PushFont(g.bold, 22);
    ImGui::TextUnformatted(Tr(S::Faq));
    ImGui::PopFont();
    for (size_t i = 0; i < kFaq.size(); i++) {
        ImGui::PushID(int(100 + i));
        if (ImGui::CollapsingHeader(Tx(kFaq[i].first))) {
            ImGui::Indent();
            ImGui::TextWrapped("%s", Tx(kFaq[i].second));
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
}

// the port's own files are there: no repository to fetch them from, or its executable in place
bool PortPresent(const Game& gm) {
    std::error_code ec;
    const std::string& file = gm.prof.port_file.empty() ? gm.prof.exe : gm.prof.port_file;
    return gm.entry.repo.empty() || fs::is_regular_file(gm.entry.dir / FromU8(file), ec);
}

// the port's files: downloaded when missing, updated when a newer release is out; false while there is nothing to play
void GameFolderLine(Game& gm, bool can_move);

bool PortCard(Game& gm) {
    if (gm.entry.repo.empty() || !Playable(gm, g.here)) return true;
    PortFetcher::Status ps = gm.fetcher.Get();
    bool present = PortPresent(gm);
    if (!gm.port_checked && !gm.fetcher.Busy()) {
        gm.port_checked = true;
        gm.fetcher.Check(gm.entry.Source(), InstalledPortVersion(gm.entry.dir));
    }
    if (gm.fetcher.Busy() && ps.stage != PortFetcher::Stage::Checking) {
        ImGui::TextColored(Accent(1.3f), "● %s", ps.stage == PortFetcher::Stage::Unpacking ? Tr(S::UnpackingPort) : Tr(S::DownloadingPort));
        char pct[48];
        float f = ps.size ? float(double(ps.done) / double(ps.size)) : 0.0f;
        std::snprintf(pct, sizeof(pct), "%.0f / %.0f MB", double(ps.done) / 1048576.0, double(ps.size) / 1048576.0);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent(0.8f));
        ImGui::ProgressBar(ps.stage == PortFetcher::Stage::Unpacking ? -float(ImGui::GetTime()) : f, ImVec2(-1, 0), pct);
        ImGui::PopStyleColor();
        if (ps.stage == PortFetcher::Stage::Downloading && ImGui::Button(Tr(S::Cancel))) gm.fetcher.Cancel();
        return false;
    }
    if (!present) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "● %s", Tr(S::PortMissing));
        ImGui::TextWrapped("%s", Tr(S::PortMissingText));
        GameFolderLine(gm, g.proc == nullptr && ps.stage != PortFetcher::Stage::Checking);
        ImGui::Spacing();
        char label[96];
        if (ps.size) std::snprintf(label, sizeof(label), Tr(S::DownloadPort), double(ps.size) / 1048576.0);
        else std::snprintf(label, sizeof(label), "%s", Tr(S::DownloadPortNoSize));
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
        if (g.focus_page) ImGui::SetKeyboardFocusHere();
        ImGui::BeginDisabled(ps.stage == PortFetcher::Stage::Checking);
        // tests: LAUNCHER_PORT_DOWNLOAD=1 presses it once the newest version is known
        static bool test_download = SDL_getenv("LAUNCHER_PORT_DOWNLOAD") != nullptr;
        bool press = test_download && ps.stage == PortFetcher::Stage::Ready;
        if (press) test_download = false;
        if (ImGui::Button(label, ImVec2(320 * g.scale, 0)) || press) gm.fetcher.Install(gm.entry.Source(), gm.entry.dir);
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
        if (ps.stage == PortFetcher::Stage::Failed)
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s: %s", Tr(S::PortFailed), ps.error.c_str());
        return false;
    }
    // installed: its version, and a newer one when there is
    std::string mine = InstalledPortVersion(gm.entry.dir);
    // a branch's code: any other commit is newer
    bool newer = mine.empty() || (gm.entry.branch.empty() ? NewerVersion(ps.latest, mine) : ps.latest != mine);
    if (ps.stage == PortFetcher::Stage::Ready && !ps.latest.empty() && newer) {
        char b[128];
        std::snprintf(b, sizeof(b), Tr(S::PortNewer), ps.latest.c_str());
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("wip")), "● %s", b);
        ImGui::SameLine();
        ImGui::BeginDisabled(g.proc != nullptr);
        if (ImGui::SmallButton(Tr(S::UpdatePort))) gm.fetcher.Install(gm.entry.Source(), gm.entry.dir);
        ImGui::EndDisabled();
    } else if (ps.stage == PortFetcher::Stage::Done) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", Tr(S::PortUpdated));
    }
    return true;
}

// an installed port with a newer version out, its tag in latest (the same test as the game's page, but quiet about a
// port whose version is unknown: a copy made by hand)
bool PortHasUpdate(Game& gm, std::string& latest) {
    if (gm.entry.repo.empty() || !gm.ok || !Playable(gm, g.here) || !PortPresent(gm)) return false;
    PortFetcher::Status ps = gm.fetcher.Get();
    if (ps.stage != PortFetcher::Stage::Ready || ps.latest.empty()) return false;
    std::string mine = InstalledPortVersion(gm.entry.dir);
    if (mine.empty()) return false;
    latest = ps.latest;
    return gm.entry.branch.empty() ? NewerVersion(ps.latest, mine) : ps.latest != mine;
}

// the launcher's and the installed ports' newest versions: on start and every 6 hours while the launcher is open
// (a few small requests to GitHub, on threads of their own)
void PeriodicChecks() {
    Uint64 now = SDL_GetTicks();
    if (g.last_check && now - g.last_check < 6ull * 3600 * 1000) return;
    bool first = g.last_check == 0;
    g.last_check = now;
    // the owner's answers to the reports sent from here
    if (!Server().empty() && g.answers_call.Get() == Call::State::Idle)
        g.answers_call.Start("GET", Server() + "/v1/respostas", ServerHeaders(), "");
    if (!g.state.Bool("check_updates", true)) return;
    UpdateCheck u = g.updater.Get();
    // on start the launcher's own look is already on its way
    if (!first && !g.hub.update_repo.empty() && u.state != UpdateCheck::State::Downloading &&
        u.state != UpdateCheck::State::Ready && u.state != UpdateCheck::State::Checking) {
        g.updater.Start(g.hub.update_repo, g.hub.version);
        g.upd_shown = false;
    }
    for (auto& gp : g.games) {
        Game& gm = *gp;
        if (gm.entry.repo.empty() || !gm.ok || !Playable(gm, g.here) || !PortPresent(gm) || gm.fetcher.Busy()) continue;
        gm.port_checked = true;
        gm.fetcher.Check(gm.entry.Source(), InstalledPortVersion(gm.entry.dir));
    }
    g.ports_shown = false;
}

void MarkdownText(const std::string& md);

// pop-ups over any page: the first measured session's question about sharing, a newer launcher, newer ports
// an answer is seen as its report and its time: an answer changed later shows again
std::string AnswerMark(const std::vector<std::string>& row) { return Str(row, 0) + ":" + Str(row, 6); }

void Notices() {
    if (g.perf_call.Get() == Call::State::Done) g.perf_call.Reset();  // a session sent: nothing to show
    HttpResult ar;
    if (g.answers_call.Get(&ar) == Call::State::Done) {
        if (ar.Ok()) {
            g.answers = Lines(ar.body);
            std::string seen = "," + g.state.Str("answers_seen", "") + ",";
            g.answers_new.clear();
            for (size_t i = 0; i < g.answers.size(); i++)
                if (!Str(g.answers[i], 5).empty() && seen.find("," + AnswerMark(g.answers[i]) + ",") == std::string::npos)
                    g.answers_new.push_back(i);
        }
        g.answers_call.Reset();
    }
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    float width = std::min(io.DisplaySize.x * 0.85f, 780 * g.scale);
    bool any_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * g.scale, 20 * g.scale));
    ImGui::PushFont(g.body, 19);

    if (g.perf_ask && !any_open) {
        ImGui::OpenPopup("perf_ask");
        any_open = true;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    if (ImGui::BeginPopupModal("perf_ask", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(g.bold, 24);
        ImGui::TextUnformatted(Tr(S::PerfAskTitle));
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Tr(S::PerfAskText));
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(Tr(S::PerfShowWhat))) {
            static std::string shown;
            shown = PrettyJson(ReportJson(ThisMachine(), g.perf_pending, g.install_id));
            ImGui::InputTextMultiline("##what", shown.data(), shown.size() + 1, ImVec2(-1, 240 * g.scale),
                                      ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
        if (ImGui::Button(Tr(S::PerfSend))) {
            g.state.Set("perf_share", std::string("sim"));
            SendSession(g.perf_pending);
            g.perf_ask = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::PerfDontSend))) {
            g.state.Set("perf_share", std::string("nao"));
            g.perf_ask = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    UpdateCheck u = g.updater.Get();
    bool launcher_new = u.state == UpdateCheck::State::Newer && !u.latest.empty() &&
                        u.latest != g.state.Str("ignored_launcher", "");
    if (launcher_new && !g.upd_shown && !any_open) {
        g.upd_shown = true;
        ImGui::OpenPopup("upd_launcher");
        any_open = true;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    if (ImGui::BeginPopupModal("upd_launcher", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(g.bold, 24);
        ImGui::TextUnformatted(Tr(S::UpdPopTitle));
        ImGui::PopFont();
        ImGui::Spacing();
        char b[160];
        std::snprintf(b, sizeof(b), Tr(S::UpdPopLauncher), u.latest.c_str());
        ImGui::TextWrapped("%s", b);
        // the new version's notes, when they are there
        g.notes.Fetch(g.hub.update_repo);
        std::vector<Release> list;
        g.notes.Get(g.hub.update_repo, &list);
        for (const Release& r : list) {
            if (r.tag != u.latest || r.body.empty()) continue;
            ImGui::Spacing();
            ImGui::BeginChild("notas", ImVec2(-1, 260 * g.scale), ImGuiChildFlags_Borders);
            MarkdownText(r.body);
            ImGui::EndChild();
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
        ImGui::BeginDisabled(u.installer.empty() || u.state != UpdateCheck::State::Newer);
        if (ImGui::Button(Tr(S::UpdateNow))) {
            g.updater.Download();  // the library's banner shows the download, then the installer runs
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::WhatsNew))) SDL_OpenURL(u.url.c_str());
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::UpdLater))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::UpdIgnore))) {
            g.state.Set("ignored_launcher", u.latest);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // the ports: those with a newer version than the one skipped
    static std::vector<std::pair<int, std::string>> ports;  // game, its newest version
    if (!g.ports_shown && !any_open) {
        std::vector<std::pair<int, std::string>> found;
        bool checking = false;
        for (size_t i = 0; i < g.games.size(); i++) {
            Game& gm = *g.games[i];
            if (gm.fetcher.Get().stage == PortFetcher::Stage::Checking) checking = true;
            std::string latest;
            std::string skipped = g.state.Str(("ignored_port_" + gm.entry.id).c_str(), "");
            if (PortHasUpdate(gm, latest) && latest != skipped) found.push_back({int(i), latest});
        }
        // once every look is back
        if (!checking) {
            g.ports_shown = true;
            if (!found.empty()) {
                ports = found;
                ImGui::OpenPopup("upd_ports");
            }
        }
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    if (ImGui::BeginPopupModal("upd_ports", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(g.bold, 24);
        ImGui::TextUnformatted(Tr(S::UpdPortsTitle));
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Tr(S::UpdPortsText));
        ImGui::Spacing();
        for (auto& [i, latest] : ports) {
            Game& gm = *g.games[size_t(i)];
            ImGui::PushID(i);
            ImGui::TextColored(Accent(1.3f), "●");
            ImGui::SameLine();
            std::string shown = latest.size() > 12 && gm.entry.branch.size() ? latest.substr(0, 7) : latest;
            ImGui::Text("%s (%s)", Tx(gm.prof.title), shown.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(S::UpdOpen))) {
                OpenGame(i);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (ImGui::Button(Tr(S::UpdLater))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::UpdIgnorePorts))) {
            for (auto& [i, latest] : ports) g.state.Set(("ignored_port_" + g.games[size_t(i)]->entry.id).c_str(), latest);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // the owner's answers to reports sent from here, each once
    if (!g.answers_new.empty() && !any_open) {
        ImGui::OpenPopup("answers");
        any_open = true;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    if (ImGui::BeginPopupModal("answers", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(g.bold, 24);
        ImGui::TextUnformatted(Tr(S::AnswerTitle));
        ImGui::PopFont();
        for (size_t i : g.answers_new) {
            if (i >= g.answers.size()) continue;
            const auto& row = g.answers[i];
            ImGui::Spacing();
            ImGui::TextColored(Accent(1.3f), "%s · %s", Str(row, 2).c_str(), Str(row, 3).c_str());
            Dim((std::string(Tr(S::AnswerYourReport)) + " \"" + Str(row, 4) + "\"").c_str());
            ImGui::TextWrapped("%s", Str(row, 5).c_str());
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
        if (ImGui::Button(Tr(S::AnswerOk))) {
            // the marks of the answers there are now: those of answers gone drop out
            std::string seen;
            for (const auto& row : g.answers) seen += (seen.empty() ? "" : ",") + AnswerMark(row);
            g.state.Set("answers_seen", seen);
            g.answers_new.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::PopFont();
    ImGui::PopStyleVar();
}

// a finished install: its game becomes the one to play, whatever page is open and even with the window minimized
void ApplyInstalls() {
    for (auto& gp : g.games) {
        Game& gm = *gp;
        if (gm.install_taken || gm.installer.Get().stage != Installer::Stage::Done) continue;
        gm.install_taken = true;
        if (const Option* o = gm.prof.game_option.empty() ? nullptr : gm.prof.Find(gm.prof.game_option)) {
            gm.store.Set(*o, U8(gm.installer.Get().game));
            gm.store.Save();
        }
        StartArt(gm);
        gm.theme_tried = false;
        PlayTheme();
    }
}

// ---------------------------------------------------------------------------------------------- errors to GitHub

std::string PercentEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += char(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            o += b;
        }
    }
    return o;
}

// an exit code as Windows writes it: crashes are NTSTATUS values such as 0xC0000005
std::string ExitText(int code) {
    char b[32];
    if (code < 0 || code > 0xFFFF) std::snprintf(b, sizeof(b), "0x%08X", unsigned(code));
    else std::snprintf(b, sizeof(b), "%d", code);
    return b;
}

// the log a report or an issue carries: a generation that stopped, the generator's own; else the profile's log_file;
// else the newest .log or .txt in its logs folder (the newest file of any kind was a generator's GOWA.imports.json
// once the game had been made again)
fs::path GameLog(Game& gm) {
    std::error_code ec;
    if (gm.prof.install.kind == "build" && gm.build.Get().stage == GameBuild::Stage::Failed) {
        fs::path b = gm.entry.dir / FromU8(gm.prof.install.marks) / "launcher.log";
        if (fs::is_regular_file(b, ec)) return b;
    }
    if (!gm.prof.log_file.empty()) {
        fs::path f = gm.entry.dir / FromU8(gm.prof.log_file);
        if (fs::is_regular_file(f, ec)) return f;
    }
    fs::path best;
    fs::file_time_type when{};
    for (auto& e : fs::directory_iterator(gm.entry.dir / FromU8(gm.prof.logs), ec)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (!e.is_regular_file(ec) || (ext != ".log" && ext != ".txt")) continue;
        auto t = e.last_write_time(ec);
        if (best.empty() || t > when) best = e.path(), when = t;
    }
    return best;
}

// the profile's report_files that exist: a name with * in it is the newest file that matches (a port that writes no
// text log, as Silent Hill's: its heartbeat and freeze dumps, its options and the mods turned on)
std::vector<fs::path> ReportFiles(Game& gm) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const std::string& f : gm.prof.report_files) {
        fs::path p = gm.entry.dir / FromU8(f);
        std::string name = U8(p.filename());
        if (name.find('*') == std::string::npos) {
            if (fs::is_regular_file(p, ec)) out.push_back(p);
            continue;
        }
        size_t star = name.find('*');
        std::string head = name.substr(0, star), tail = name.substr(star + 1);
        fs::path best;
        fs::file_time_type when{};
        for (auto& e : fs::directory_iterator(p.parent_path(), ec)) {
            std::string n = U8(e.path().filename());
            if (!e.is_regular_file(ec) || n.size() < head.size() + tail.size() || n.compare(0, head.size(), head) != 0 ||
                n.compare(n.size() - tail.size(), tail.size(), tail) != 0)
                continue;
            auto t = e.last_write_time(ec);
            if (best.empty() || t > when) best = e.path(), when = t;
        }
        if (!best.empty()) out.push_back(best);
    }
    return out;
}

// a new issue on the port's repository (the launcher's for a game without one), filled in: what happened, the
// computer, the versions, the settings and the end of the log, the log shortened until the address is under limit
struct Issue {
    std::string repo, title, body, url;
};

Issue MakeIssue(Game& gm, const std::string& kind, const std::string& what, size_t limit) {
    if (g.sysinfo.empty()) g.sysinfo = SystemInfo();
    std::string repo = gm.entry.repo.empty() ? g.hub.update_repo : gm.entry.repo;
    std::string port = InstalledPortVersion(gm.entry.dir);
    std::string head = "**" + kind + "**\n\n" + what + "\n\n| | |\n|---|---|\n";
    head += "| Jogo | " + gm.prof.title.Get(Lang::Pt) + " (`" + gm.entry.id + "`) |\n";
    head += "| Versão do port | " + (port.empty() ? std::string("-") : port) + " |\n";
    head += "| Launcher | " + g.hub.version + " · " + PlatformName(g.here) + " |\n";
    for (auto& [k, v] : g.sysinfo) head += "| " + k + " | " + v + " |\n";
    if (gm.last_exit != INT_MIN) {
        char b[96];
        std::snprintf(b, sizeof(b), "| Última saída | código %s depois de %.0f min |\n", ExitText(gm.last_exit).c_str(), gm.last_minutes);
        head += b;
    }
    std::string config = FileTail(gm.entry.dir / FromU8(gm.prof.settings), 4096);
    fs::path log = GameLog(gm);
    std::string tail = log.empty() ? std::string() : FileTail(log, 64 * 1024);
    Issue is{repo, gm.prof.title.Get(Lang::Pt) + ": " + kind};
    auto build = [&](const std::string& logpart) {
        is.body = head;
        if (!config.empty()) is.body += "\n<details><summary>Configurações</summary>\n\n```toml\n" + config + "\n```\n</details>\n";
        if (!logpart.empty())
            is.body += "\n<details><summary>Final do log (" + U8(log.filename()) + ")</summary>\n\n```\n" + logpart + "\n```\n</details>\n";
        is.body += "\nSe houver arquivos na pasta `CrashDumps` do port, compacte e arraste para cá.";
        is.url = "https://github.com/" + repo + "/issues/new?title=" + PercentEncode(is.title) + "&body=" + PercentEncode(is.body);
    };
    build(tail);
    while (is.url.size() > limit && !tail.empty()) {
        size_t cut = tail.find('\n', tail.size() / 4);
        tail = cut == std::string::npos ? std::string() : tail.substr(cut + 1);
        build(tail);
    }
    return is;
}

// the address GitHub takes from a player who is not logged in: its login page carries the whole address, and past
// about 7 000 characters GitHub answers that with an error 500 (checked: 6 000 comes back whole after the login)
constexpr size_t kLongIssue = 6000;
std::string IssueUrl(Game& gm, const std::string& kind, const std::string& what) { return MakeIssue(gm, kind, what, kLongIssue).url; }

// past this an address may not reach the page: Windows hands it to the browser through the command the browser
// registered, and some browsers or systems cut it or refuse it
constexpr size_t kShortIssue = 2000;

// the issue in the browser. A long report goes by the clipboard: the page opens with the title and a line asking to
// paste it. When the system does not open the browser, the player is told, with the address (or the report) copied.
void OpenIssue(Game& gm, const std::string& kind, const std::string& what) {
    Issue is = MakeIssue(gm, kind, what, kLongIssue);
    // tests: LAUNCHER_ISSUE_FILE=path keeps the address there instead of opening the browser
    if (const char* f = SDL_getenv("LAUNCHER_ISSUE_FILE")) {
        std::ofstream(f) << is.url;
        return;
    }
    std::string url = is.url;
    bool pasted = false;
    if (url.size() > kShortIssue) {
        // GitHub takes a body of up to 65 536 characters: the log can be longer this way
        Issue full = MakeIssue(gm, kind, what, 120000);
        pasted = SDL_SetClipboardText(full.body.c_str());
        if (pasted)
            url = "https://github.com/" + is.repo + "/issues/new?title=" + PercentEncode(is.title) + "&body=" +
                  PercentEncode("O launcher copiou o relatório completo: apague esta linha e cole aqui (Ctrl+V).");
    }
    if (SDL_OpenURL(url.c_str())) {
        if (pasted) Toast(Tr(S::IssueCopied), 9000);
        // a full-screen launcher would stay over the browser
        if (g.state.Str("window_mode", "window") == "fullscreen") SDL_MinimizeWindow(g.win);
        return;
    }
    char b[320];
    if (pasted) {
        std::snprintf(b, sizeof(b), Tr(S::IssueNoBrowserReport), is.repo.c_str());
    } else {
        SDL_SetClipboardText(url.c_str());
        std::snprintf(b, sizeof(b), "%s", Tr(S::IssueNoBrowser));
    }
    Toast(b, 12000);
}

// the game's report page, filled in from an error card: "Report to the launcher" is the main way (no account needed;
// the owner reads it and answers in the launcher), GitHub the other. cat is kCats' place: 0 crash, 5 other
void OpenReport(Game& gm, int cat, const std::string& desc) {
    g.rep_cat = cat;
    SDL_strlcpy(g.rep_desc, desc.c_str(), sizeof(g.rep_desc));
    g.rep_attach = true;
    g.rep_msg.clear();
    auto entries = Entries();
    for (size_t k = 0; k < entries.size(); k++)
        if (entries[k].kind == Kind::Report) gm.page = int(k);
    g.focus_page = true;
}

void CrashCard(Game& gm) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g.scale, 14 * g.scale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.09f, 0.08f, 0.92f));
    ImGui::BeginChild("crash", ImVec2(-1, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
    char b[256];
    std::snprintf(b, sizeof(b), Tr(S::CrashTitle), ExitText(gm.last_exit).c_str(), gm.last_minutes);
    ImGui::PushFont(g.bold, 21);
    ImGui::TextWrapped("%s", b);
    ImGui::PopFont();
    ImGui::TextWrapped("%s", Tr(S::CrashText));
    ImGui::Spacing();
    std::snprintf(b, sizeof(b), "O jogo fechou com erro (código %s) depois de %.0f minutos.", ExitText(gm.last_exit).c_str(),
                  gm.last_minutes);
    ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
    if (ImGui::Button(Tr(S::ReportToLauncher))) {
        OpenReport(gm, 0, std::string(b) + "\n\nO que eu estava fazendo: ");
        gm.crashed = false;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(Tr(S::SendToGitHub))) {
        OpenIssue(gm, "Crash", std::string(b) + "\n\n**O que eu estava fazendo:** (conte aqui)");
        gm.crashed = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(S::Dismiss))) gm.crashed = false;
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------------------------- made on this PC

// a folder or file picked for a build's input, from the dialog's thread
struct BuildPick {
    std::mutex m;
    int game = -1;
    std::string key, path;
    bool ready = false;
} g_build_pick;
// the note on a folder picked for a required input, "game/key": 1 the port's own folder (refused), 2 the folder
// holding the games (kept, with a warning)
std::map<std::string, int> g_pick_note;

// where a folder picked for a required input (the game's disc) stands against the port. Reports 33 and 34: the
// dialog opens where Windows last was, often the folder just picked for the games, and two players took that as
// the disc. 1: the port's own folder or one inside it, never the game; 2: the folder the games are installed in,
// where the game may still be in a folder of its own; 0: elsewhere.
int PickedPortPlace(const Game& gm, const std::string& picked) {
    std::error_code ec;
    if (!fs::is_directory(FromU8(picked), ec)) return 0;
    fs::path p = fs::weakly_canonical(FromU8(picked), ec);
    if (ec) return 0;
    fs::path d = fs::weakly_canonical(gm.entry.dir, ec);
    if (ec || d.empty()) return 0;
    auto low = [](fs::path x) {
        std::wstring s = x.make_preferred().wstring();
        while (s.size() > 3 && s.back() == L'\\') s.pop_back();
        for (wchar_t& c : s) c = wchar_t(towlower(c));
        return s;
    };
    const std::wstring ps = low(p), ds = low(d);
    if (ps == ds || (ps.size() > ds.size() && ps.compare(0, ds.size(), ds) == 0 && ps[ds.size()] == L'\\')) return 1;
    return ps == low(d.parent_path()) ? 2 : 0;
}

void SDLCALL BuildPicked(void*, const char* const* list, int) {
    if (!list || !list[0]) return;
    std::lock_guard lock(g_build_pick.m);
    g_build_pick.path = list[0];
    g_build_pick.ready = true;
}

// the next small button on the same line when it fits there, otherwise on the next
void SameLineIfFits(const char* label) {
    float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    float right = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width;
    if (right <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
}

// an input's file fetched from an official server (BuildInput::download), one at a time per game and input
struct InputFetch {
    std::mutex m;
    std::atomic<bool> busy{false}, cancel{false};
    uint64_t done = 0;
    std::string error;
    std::string ready;  // the file, once checked: taken into the input's value on the UI thread
};
std::map<std::string, std::unique_ptr<InputFetch>> g_input_fetch;  // "game/key"

InputFetch& FetchOf(const Game& gm, const BuildInput& in) {
    auto& p = g_input_fetch[gm.entry.id + "/" + in.key];
    if (!p) p = std::make_unique<InputFetch>();
    return *p;
}

// into the port's dl folder; a file already there with the right SHA-1 is taken as it is
void StartInputFetch(Game& gm, const BuildInput& in) {
    InputFetch& f = FetchOf(gm, in);
    if (f.busy.exchange(true)) return;
    f.cancel = false;
    {
        std::lock_guard lock(f.m);
        f.done = 0;
        f.error.clear();
        f.ready.clear();
    }
    fs::path file = gm.entry.dir / "dl" / FromU8(in.download_file);
    std::string url = in.download_url, sha1 = in.download_sha1;
    std::thread([&f, file, url, sha1] {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::string err;
        bool ok = fs::is_regular_file(file, ec) && FileSha1(file) == sha1;
        if (!ok) {
            fs::path part = file;
            part += ".parcial";
            std::string sha256;
            ok = DownloadFile(url, part, f.cancel, f.done, f.m, sha256, err);
            if (ok && FileSha1(part) != sha1) {
                ok = false;
                err = "o arquivo baixado não confere (SHA-1)";
            }
            if (ok) {
                fs::rename(part, file, ec);
                if (ec) ok = false, err = ec.message();
            } else {
                fs::remove(part, ec);
            }
        }
        std::lock_guard lock(f.m);
        if (ok) f.ready = U8(file);
        else f.error = err;
        f.busy = false;
    }).detach();
}

// a folder the generators can take: no spaces (the commands through cmd and the PS3 tools break on them) and nothing
// outside ASCII
bool GoodGamesFolder(const std::string& path) {
    for (unsigned char c : path)
        if (c == ' ' || c >= 0x80) return false;
    return !path.empty();
}

// the folder the games are installed in (Settings > General), one folder each; empty: the hub's own places (also for
// a path the generators could not take, written by hand)
fs::path GamesFolder() {
    std::string f = g.state.Str("games_folder", "");
    return GoodGamesFolder(f) ? FromU8(f).make_preferred() : fs::path();
}

// where a game lives: pinned once installed away from the hub's place; else, for a port not installed yet, the games
// folder; else the hub's place. A game the hub keeps beside the launcher, or one with no port to fetch, stays there.
void PlaceGame(Game& gm) {
    fs::path home = gm.entry.dir;
    for (const GameEntry& e : g.hub.games)
        if (e.id == gm.entry.id) home = e.dir;
    gm.entry.dir = home;
    std::string pinned = g.state.Str(("game_dir_" + gm.entry.id).c_str(), "");
    if (!pinned.empty()) {
        gm.entry.dir = FromU8(pinned);
        return;
    }
    fs::path base = GamesFolder();
    if (base.empty() || gm.entry.beside || gm.entry.repo.empty() || PortPresent(gm)) return;
    gm.entry.dir = base / FromU8(gm.entry.id);
}

void OpenGameFolder(Game& gm);

// a game installed away from the hub's place keeps its folder whatever the games folder becomes later
void PinInstalledGames() {
    for (auto& p : g.games) {
        Game& gm = *p;
        fs::path home = gm.entry.dir;
        for (const GameEntry& e : g.hub.games)
            if (e.id == gm.entry.id) home = e.dir;
        if (gm.entry.dir != home && PortPresent(gm)) g.state.Set(("game_dir_" + gm.entry.id).c_str(), U8(gm.entry.dir));
    }
}

// the player's own folder for one game, picked on its page before anything was made there: the game goes to
// <picked>\<id>, pinned. A port already downloaded moves with it, but only a small one with nothing made, no link
// inside (GoW's junction to the player's disc must never be followed by a copy or a delete) and an empty destination.
bool MoveGameTo(Game& gm, const std::string& picked) {
    if (!GoodGamesFolder(picked)) {
        Toast(Tr(S::GamesFolderBad), 7000);
        return false;
    }
    fs::path dest = (FromU8(picked) / FromU8(gm.entry.id)).make_preferred();
    fs::path from = gm.entry.dir;
    std::error_code ec;
    if (dest == from) return true;
    if (PortPresent(gm)) {
        uintmax_t size = 0;
        bool link = false;
        for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            fs::file_type t = it->symlink_status(ec).type();
            if (t == fs::file_type::symlink || t == fs::file_type::junction) {
                link = true;
                break;
            }
            if (t == fs::file_type::regular) size += it->file_size(ec);
        }
        bool made_some = false;
        if (!gm.prof.install.marks.empty())
            for (auto& e : fs::directory_iterator(from / FromU8(gm.prof.install.marks), ec))
                made_some |= e.path().extension() == ".ok";
        if (link || made_some || size > (uintmax_t(2) << 30)) {
            Toast(Tr(S::GameFolderBusy), 8000);
            return false;
        }
        if (fs::exists(dest, ec) && !fs::is_empty(dest, ec)) {
            Toast(Tr(S::GameFolderNotEmpty), 7000);
            return false;
        }
        fs::create_directories(dest.parent_path(), ec);
        fs::remove(dest, ec);  // an empty one, for the rename
        ec.clear();
        fs::rename(from, dest, ec);
        if (ec) {  // another drive: copied, and the old folder removed only once the copy is whole
            ec.clear();
            fs::copy(from, dest, fs::copy_options::recursive, ec);
            if (ec) {
                Toast(std::string(Tr(S::GameFolderMoveFailed)) + ec.message(), 8000);
                fs::remove_all(dest, ec);
                return false;
            }
            fs::remove_all(from, ec);
        }
    }
    gm.entry.dir = dest;
    g.state.Set(("game_dir_" + gm.entry.id).c_str(), U8(dest));
    OpenGameFolder(gm);
    gm.port_checked = false;  // its installed version read again, from the new folder
    return true;
}

// the folder picked for a game: the dialog answers on its own thread
std::mutex g_game_folder_m;
std::string g_game_folder_pick, g_game_folder_id;

void GameFolderPicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lock(g_game_folder_m);
    g_game_folder_pick = files[0];
}

// where this game is installed, and, while nothing was made there (can_move), the player's own folder for it
void GameFolderLine(Game& gm, bool can_move) {
    std::string picked;
    {
        std::lock_guard lock(g_game_folder_m);
        if (g_game_folder_id == gm.entry.id && !g_game_folder_pick.empty()) {
            picked.swap(g_game_folder_pick);
            g_game_folder_id.clear();
        }
    }
    if (!picked.empty() && can_move) MoveGameTo(gm, picked);
    std::string at = std::string(Tr(S::GameFolderAt)) + " " + U8(gm.entry.dir);
    Dim(at.c_str());  // wrapped: a long path stays on the page
    if (!can_move) return;
    SameLineIfFits(Tr(S::ChangeFolder));
    if (ImGui::SmallButton(Tr(S::ChangeFolder))) {
        {
            std::lock_guard lock(g_game_folder_m);
            g_game_folder_id = gm.entry.id;
            g_game_folder_pick.clear();
        }
        static std::string start;
        start = U8(gm.entry.dir.parent_path());
        SDL_ShowOpenFolderDialog(GameFolderPicked, nullptr, g.win, start.c_str(), false);
    }
}

// the folder the player keeps the games' discs in (Settings > General): the disc pickers open there, and a game's disc
// is looked for there by its name; empty when not set or gone
fs::path DiscsFolder() {
    std::string f = g.state.Str("discs_folder", "");
    std::error_code ec;
    return !f.empty() && fs::is_directory(FromU8(f), ec) ? FromU8(f).make_preferred() : fs::path();
}

// a name's words, lower case, apostrophes dropped: "Dante's Inferno" and "dantes inferno" are the same
std::vector<std::string> NameWords(const std::string& s) {
    std::vector<std::string> words;
    std::string w;
    for (unsigned char c : s) {
        if (c == '\'') continue;
        if (std::isalnum(c) || c >= 0x80) {
            w += char(std::tolower(c));
        } else if (!w.empty()) {
            words.push_back(w);
            w.clear();
        }
    }
    if (!w.empty()) words.push_back(w);
    return words;
}

// the game's disc in the discs folder or a folder inside it: the one entry the input takes (a file with its
// extensions, or a folder holding its mark) whose name, up to the first "(" or "[" (the region, the languages), is
// the game's title: "Saints Row (World) (En,Fr,De,Es,It).iso" for Saints Row, never "Army of Two - The 40th Day" for
// Army of Two. Empty when none or more than one: then the player picks. The generator still checks the version.
std::string FindDisc(const Game& gm, const BuildInput& in) {
    fs::path dir = DiscsFolder();
    if (dir.empty()) return {};
    std::vector<std::string> title = NameWords(gm.prof.title.Get(Lang::En));
    if (title.empty()) return {};
    std::vector<fs::path> found;
    std::error_code ec;
    auto consider = [&](const fs::path& p, bool is_dir) {
        if (is_dir) {
            if (!in.folder || in.unpack.empty() || !fs::exists(p / FromU8(in.unpack), ec)) return;
        } else {
            if (!in.file) return;
            std::string ext = U8(p.extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (std::find(in.filters.begin(), in.filters.end(), ext.empty() ? ext : ext.substr(1)) == in.filters.end())
                return;
        }
        std::string name = U8(is_dir ? p.filename() : p.stem());
        name = name.substr(0, name.find_first_of("(["));
        if (NameWords(name) == title) found.push_back(p);
    };
    for (auto& e : fs::directory_iterator(dir, ec)) {
        bool is_dir = e.is_directory(ec);
        consider(e.path(), is_dir);
        if (is_dir)
            for (auto& inner : fs::directory_iterator(e.path(), ec)) consider(inner.path(), inner.is_directory(ec));
    }
    return found.size() == 1 ? U8(found[0]) : std::string();
}

void PickBuildInput(Game& gm, const BuildInput& in, bool file) {
    {
        std::lock_guard lock(g_build_pick.m);
        g_build_pick.game = g.cur;
        g_build_pick.key = in.key;
        g_build_pick.ready = false;
    }
    std::string start = gm.store.GetOwn(in.key);
    if (start.empty()) start = U8(DiscsFolder());
    if (file) {
        static std::string name, pattern;
        name = Tx(in.label);
        pattern.clear();
        for (const std::string& e : in.filters) pattern += (pattern.empty() ? "" : ";") + e;
        static SDL_DialogFileFilter filter;
        filter = {name.c_str(), pattern.empty() ? "*" : pattern.c_str()};
        SDL_ShowOpenFileDialog(BuildPicked, nullptr, g.win, &filter, 1, start.empty() ? nullptr : start.c_str(), false);
    } else {
        SDL_ShowOpenFolderDialog(BuildPicked, nullptr, g.win, start.empty() ? nullptr : start.c_str(), false);
    }
}

// the tools the generator needs, checked on a thread
void CheckPrereqs(Game& gm) {
    {
        std::lock_guard lock(gm.prereq_m);
        gm.prereq.assign(gm.prof.install.prereqs.size(), -1);
    }
    std::vector<Prereq> list = gm.prof.install.prereqs;
    std::thread([&gm, list] {
        for (size_t i = 0; i < list.size(); i++) {
            std::string out;
            int code = RunHidden(ExpandEnv(list[i].check), &out, 30000);
            bool ok = code == 0 && (!list[i].output || out.find_first_not_of(" \r\n\t") != std::string::npos);
            std::lock_guard lock(gm.prereq_m);
            if (i < gm.prereq.size()) gm.prereq[i] = ok ? 1 : 0;
        }
    }).detach();
}

void StartBuild(Game& gm, bool redo) {
    const InstallSpec& in = gm.prof.install;
    std::vector<std::string> args;
    auto expand = [&](const std::string& a) {
        std::string out = a;
        for (size_t at; (at = out.find("{dir}")) != std::string::npos;) out.replace(at, 5, U8(gm.entry.dir));
        for (const BuildInput& b : in.inputs)
            for (size_t at; (at = out.find("{" + b.key + "}")) != std::string::npos;)
                out.replace(at, b.key.size() + 2, gm.store.GetOwn(b.key));
        return out;
    };
    for (const std::string& a : in.command) {
        // an optional input left empty: its argument out, and the flag before it ("-Update", "{update}")
        bool left_out = false;
        for (const BuildInput& b : in.inputs)
            if (!b.auto_text.Empty() && a == "{" + b.key + "}" && gm.store.GetOwn(b.key).empty()) left_out = true;
        if (left_out) {
            if (!args.empty() && args.back().rfind("-", 0) == 0) args.pop_back();
            continue;
        }
        args.push_back(expand(a));
    }
    if (redo)
        for (const std::string& a : in.redo) args.push_back(expand(a));
    std::vector<std::string> steps;
    for (const BuildStep& s : in.steps) steps.push_back(s.id);
    gm.build_noted = false;
    gm.build.Start(args, gm.entry.dir / FromU8(in.marks), steps);
}

// what an unpacking input points at, to unpack: the archive or disc image picked, or the one alone in the folder
// picked; empty when the folder already holds the mark (or a folder inside it does: that one becomes the input)
fs::path UnpackSource(Game& gm, const BuildInput& b) {
    std::error_code ec;
    fs::path v = FromU8(gm.store.GetOwn(b.key));
    if (b.unpack.empty() || v.empty()) return {};
    if (fs::is_regular_file(v, ec)) return IsArchive(v) || IsDiscImage(v) ? v : fs::path();
    if (!fs::is_directory(v, ec) || fs::is_regular_file(v / FromU8(b.unpack), ec)) return {};
    // the folder above the game's: the game's folder, two levels down at most
    for (auto it = fs::recursive_directory_iterator(v, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > 1) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec) && fs::is_regular_file(it->path() / FromU8(b.unpack), ec)) {
            gm.store.SetOwn(b.key, U8(it->path()));
            gm.store.Save();
            return {};
        }
    }
    // else the download sitting in it
    fs::path only;
    for (auto& e : fs::directory_iterator(v, ec))
        if (e.is_regular_file(ec) && (IsArchive(e.path()) || IsDiscImage(e.path()))) {
            if (!only.empty()) return {};  // which one? the player picks the file
            only = e.path();
        }
    return only;
}

void StartUnpack(Game& gm, const BuildInput& b, const fs::path& source) {
    InstallSpec spec;
    spec.check = b.unpack;
    spec.unpack_images = true;
    gm.unpack_key = b.key;
    gm.unpack_error.clear();
    gm.installer.Start(source, gm.entry.dir / FromU8(b.key), spec);
}

// the generator, after the inputs given as archives or disc images are unpacked (false: nothing to unpack)
bool UnpackInputs(Game& gm, bool redo) {
    for (const BuildInput& b : gm.prof.install.inputs) {
        fs::path source = UnpackSource(gm, b);
        if (source.empty()) continue;
        gm.unpack_redo = redo;
        StartUnpack(gm, b, source);
        return true;
    }
    return false;
}

void BuildOrUnpack(Game& gm, bool redo) {
    if (!UnpackInputs(gm, redo)) StartBuild(gm, redo);
}

// an input being unpacked, done: the folder found is the input and the generator goes on (true while there is still
// unpacking or the generator was started; the game's page and "update and make all" both move it on)
bool UnpackStep(Game& gm) {
    if (gm.unpack_key.empty()) return false;
    if (gm.installer.Busy()) return true;
    Installer::Status st = gm.installer.Get();
    std::error_code ec;
    const BuildInput* input = nullptr;
    for (const BuildInput& b : gm.prof.install.inputs)
        if (b.key == gm.unpack_key) input = &b;
    if (st.stage == Installer::Stage::Failed || !input) {
        gm.unpack_error = st.error;
        gm.unpack_key.clear();
        if (!gm.unpack_image.empty()) fs::remove(FromU8(gm.unpack_image), ec);
        gm.unpack_image.clear();
        return false;
    }
    if (st.stage != Installer::Stage::Done) return false;
    // a disc image inside the archive: its files in turn, the image itself not kept
    if (fs::is_regular_file(st.game, ec) && IsDiscImage(st.game)) {
        gm.unpack_image = U8(st.game);
        StartUnpack(gm, *input, st.game);
        return true;
    }
    if (!gm.unpack_image.empty()) fs::remove(FromU8(gm.unpack_image), ec);
    gm.unpack_image.clear();
    gm.store.SetOwn(input->key, U8(st.game));
    gm.store.Save();
    gm.unpack_key.clear();
    BuildOrUnpack(gm, gm.unpack_redo);
    return true;
}

// an input being unpacked: its progress; done, the folder found is the input and the generator goes on
bool UnpackCard(Game& gm) {
    if (gm.unpack_key.empty()) return false;
    Installer::Status st = gm.installer.Get();
    if (gm.installer.Busy()) {
        const char* what = st.stage == Installer::Stage::Extracting ? Tr(S::Unpacking)
                           : st.stage == Installer::Stage::Checking ? Tr(S::CheckingVersion)
                                                                    : Tr(S::ReadingArchive);
        ImGui::TextColored(Accent(1.3f), "● %s", what);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.0f%%", st.progress * 100);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent(0.8f));
        ImGui::ProgressBar(st.stage == Installer::Stage::Extracting ? st.progress : -float(ImGui::GetTime()), ImVec2(-1, 0),
                           st.stage == Installer::Stage::Extracting ? pct : "");
        ImGui::PopStyleColor();
        Dim(Tr(S::UnpackHelp));
        if (ImGui::Button(Tr(S::Cancel))) gm.installer.Cancel();
        return true;
    }
    return UnpackStep(gm);
}

// every input the generator needs picked (an optional one, left on its "automatic", is not needed)
bool InputsSet(Game& gm) {
    for (const BuildInput& b : gm.prof.install.inputs)
        if (b.auto_text.Empty() && gm.store.GetOwn(b.key).empty()) return false;
    return true;
}

// A port made on this PC, updated to a version whose only changes are notes (*.md: NOVIDADES.md, README.md): the game
// made before has the same code, so it is taken as made with the new version -- no "make it again" and no question
// on Play for a text change (the Silent Hill's NOVIDADES.md was a commit of its own on the branch the port follows).
// GitHub's compare of the two versions, on a thread, once per pair. 0 = nothing to check, 1 = checking,
// 2 = notes only (built_version moved to the port's now), 3 = code changed or unknown.
int NotesOnlyUpdate(Game& gm) {
    if (gm.prof.install.kind != "build" || gm.entry.repo.empty()) return 0;
    const std::string port = InstalledPortVersion(gm.entry.dir), built = gm.store.GetOwn("built_version");
    if (port.empty() || built.empty() || built == port) return 0;
    static std::mutex m;
    static std::map<std::string, int> state;
    const std::string key = gm.entry.repo + "|" + built + "|" + port;
    int s;
    {
        std::lock_guard lock(m);
        s = state[key];
        if (!s) state[key] = 1;
    }
    if (!s) {
        std::thread([key, repo = gm.entry.repo, built, port] {
            HttpResult r = GitHubApi("repos/" + repo + "/compare/" + built + "..." + port);
            int res = 3;
            size_t files = r.Ok() ? r.body.find("\"files\":") : std::string::npos;
            if (files != std::string::npos) {
                int n = 0, notes = 0;
                for (size_t at = r.body.find("\"filename\":", files); at != std::string::npos;
                     at = r.body.find("\"filename\":", at + 11)) {
                    std::string f = JsonString(r.body.substr(at, 600), "filename");
                    n++;
                    if (f.size() > 3 && f.compare(f.size() - 3, 3, ".md") == 0) notes++;
                }
                if (n > 0 && n == notes) res = 2;
            }
            std::lock_guard lock(m);
            state[key] = res;
        }).detach();
        return 1;
    }
    if (s == 2) {
        gm.store.SetOwn("built_version", port);
        gm.store.Save();
    }
    return s;
}

// a finished build: the port's version it was made with kept, once (true then)
bool NoteBuild(Game& gm) {
    if (gm.build.Get().stage != GameBuild::Stage::Done || gm.build_noted) return false;
    gm.build_noted = true;
    gm.store.SetOwn("built_version", InstalledPortVersion(gm.entry.dir));
    gm.store.Save();
    return true;
}

void BuildCard(Game& gm) {
    const InstallSpec& in = gm.prof.install;
    GameBuild::Status bs = gm.build.Get();
    std::error_code ec;
    bool made = fs::is_regular_file(gm.entry.dir / FromU8(in.done), ec);
    NotesOnlyUpdate(gm);
    std::string port = InstalledPortVersion(gm.entry.dir), built = gm.store.GetOwn("built_version");
    {
        std::lock_guard lock(g_build_pick.m);
        if (g_build_pick.ready && g_build_pick.game == g.cur) {
            bool required = false;
            for (const BuildInput& b : in.inputs) required |= b.key == g_build_pick.key && b.auto_text.Empty();
            const int place = required ? PickedPortPlace(gm, g_build_pick.path) : 0;
            g_pick_note[gm.entry.id + "/" + g_build_pick.key] = place;
            if (place != 1) {
                gm.store.SetOwn(g_build_pick.key, g_build_pick.path);
                gm.store.Save();
            }
            g_build_pick.ready = false;
        }
    }
    if (UnpackCard(gm)) return;
    if (bs.stage == GameBuild::Stage::Running) {
        int total = int(in.steps.size()), cur = std::min(bs.step, total - 1);
        char head[256];
        std::snprintf(head, sizeof(head), Tr(S::BuildRunning), bs.step + 1 > total ? total : bs.step + 1, total,
                      cur >= 0 ? Tx(in.steps[size_t(cur)].label) : "");
        ImGui::TextColored(Accent(1.3f), "● %s", head);
        uint64_t s = (SDL_GetTicks() - bs.started) / 1000;
        char t[32];
        std::snprintf(t, sizeof(t), "%u:%02u", unsigned(s / 60), unsigned(s % 60));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent(0.8f));
        ImGui::ProgressBar(total ? float(bs.step) / float(total) : 0.0f, ImVec2(-1, 0), t);
        ImGui::PopStyleColor();
        if (!bs.line.empty()) ImGui::TextDisabled("%s", bs.line.c_str());
        Dim(Tr(S::BuildMinimize));
        if (ImGui::Button(Tr(S::Cancel))) gm.build.Cancel();
        return;
    }
    if (NoteBuild(gm)) {
        built = port;
        Toast(Tr(S::BuildDoneTitle));
    }
    // made with no version kept (state from another launcher, or lost): taken as made with the one installed
    if (made && built.empty() && !port.empty()) {
        gm.store.SetOwn("built_version", port);
        gm.store.Save();
        built = port;
    }
    bool inputs_set = InputsSet(gm);
    if (made && bs.stage != GameBuild::Stage::Failed && !gm.build_form) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "● %s", Tr(S::BuildDoneTitle));
        ImGui::TextDisabled("%s", U8(FindGame(gm)).c_str());
        // the player's copy gone from where it was picked: picked again (the form's button runs the generator with
        // nothing to redo, and it remakes the link)
        if (CopyMissing(gm)) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "● %s", Tr(S::BuildCopyMissing));
            ImGui::TextWrapped("%s", Tr(S::BuildCopyMissingHelp));
            // the disc in the discs folder (moved there): one click takes it, the generator runs with nothing to redo
            static std::map<std::string, std::pair<Uint64, std::vector<std::pair<std::string, std::string>>>> seen;
            auto& [at, discs] = seen[gm.entry.id];
            if (Uint64 now = SDL_GetTicks(); at == 0 || now - at > 5000) {
                at = now;
                discs.clear();
                for (const BuildInput& b : in.inputs)
                    if (std::string d = FindDisc(gm, b); !d.empty() && d != gm.store.GetOwn(b.key))
                        discs.emplace_back(b.key, d);
            }
            for (auto& [key, d] : discs) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "● %s %s", Tr(S::DiscFoundAt), d.c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
            ImGui::BeginDisabled(g.proc != nullptr);
            if (!discs.empty()) {
                if (ImGui::Button(Tr(S::UseFoundDisc))) {
                    for (auto& [key, d] : discs) gm.store.SetOwn(key, d);
                    gm.store.Save();
                    discs.clear();
                    BuildOrUnpack(gm, false);
                }
                ImGui::SameLine();
            }
            if (ImGui::Button(Tr(S::BuildRepick))) gm.build_form = true;
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
            return;
        }
        // newer code of the port than the one the game was made with: made again, from the start
        if (!port.empty() && built != port) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("wip")), "● %s", Tr(S::BuildNewCode));
            ImGui::SameLine();
            ImGui::BeginDisabled(g.proc != nullptr);
            if (ImGui::SmallButton(Tr(S::BuildRedo))) {
                if (inputs_set) BuildOrUnpack(gm, true);
                else gm.build_form = true;  // the folders first
            }
            ImGui::EndDisabled();
        }
        if (gm.last_exit != INT_MIN) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s: %.0f %s, %s %d", Tr(S::LastExit), gm.last_minutes, Tr(S::Minutes), Tr(S::ExitCode),
                                gm.last_exit);
        }
        return;
    }
    // tests: LAUNCHER_BUILD_INPUTS="key=path;key=path" fills the folders, LAUNCHER_BUILD=1 then presses the button
    if (static bool filled = false; !filled) {
        filled = true;
        if (const char* list = SDL_getenv("LAUNCHER_BUILD_INPUTS")) {
            std::string all = list;
            for (size_t at = 0; at < all.size();) {
                size_t end = all.find(';', at), eq = all.find('=', at);
                if (end == std::string::npos) end = all.size();
                if (eq != std::string::npos && eq < end) gm.store.SetOwn(all.substr(at, eq - at), all.substr(eq + 1, end - eq - 1));
                at = end + 1;
            }
            gm.store.Save();
        }
        // tests: LAUNCHER_PICK="key=path" stands for a folder picked in the dialog (and opens the folders form)
        if (const char* pick = SDL_getenv("LAUNCHER_PICK"); pick && std::strchr(pick, '=')) {
            std::lock_guard lock(g_build_pick.m);
            g_build_pick.game = g.cur;
            g_build_pick.key.assign(pick, std::strchr(pick, '='));
            g_build_pick.path = std::strchr(pick, '=') + 1;
            g_build_pick.ready = true;
            gm.build_form = true;
        }
    }
    static bool test_build = SDL_getenv("LAUNCHER_BUILD") != nullptr;
    // not made yet: what it needs, then the player's folders
    ImGui::PushFont(g.bold, 21);
    ImGui::TextUnformatted(Tr(S::BuildTitle));
    ImGui::PopFont();
    ImGui::TextWrapped("%s", Tr(S::BuildIntro));
    GameFolderLine(gm, g.proc == nullptr && bs.stage != GameBuild::Stage::Running);
    if (!in.time.Empty()) Dim(Tx(in.time));
    if (!in.version.Empty()) {
        ImGui::TextDisabled("%s:", Tr(S::NeededVersion));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", Tx(in.version));
    }
    ImGui::Spacing();
    bool tools_ok = true;
    if (!in.prereqs.empty()) {
        if (!gm.prereq_asked) {
            gm.prereq_asked = true;
            CheckPrereqs(gm);
        }
        std::vector<int> state;
        {
            std::lock_guard lock(gm.prereq_m);
            state = gm.prereq;
        }
        ImGui::TextUnformatted(Tr(S::BuildNeeds));
        ImGui::Indent();
        for (size_t i = 0; i < in.prereqs.size(); i++) {
            const Prereq& p = in.prereqs[i];
            int st = i < state.size() ? state[i] : -1;
            tools_ok &= st == 1;
            ImGui::PushID(int(i));
            ImVec4 c = st == 1 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : st == 0 ? ImVec4(0.95f, 0.45f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
            ImGui::TextColored(c, "● %s — %s", Tx(p.name), st == 1 ? Tr(S::PrereqOk) : st == 0 ? Tr(S::PrereqMissing) : Tr(S::PrereqChecking));
            if (st == 0) {
                if (!p.help.Empty()) Dim(Tx(p.help));
                if (!p.install.empty() && ImGui::SmallButton(Tr(S::PrereqInstall))) RunInConsole(p.install, Tx(p.name));
            }
            ImGui::PopID();
        }
        ImGui::Unindent();
        if (!tools_ok && ImGui::SmallButton(Tr(S::PrereqRecheck))) CheckPrereqs(gm);
        ImGui::Spacing();
    }
    bool inputs_ok = true;
    ImGui::TextUnformatted(Tr(S::BuildInputs));
    ImGui::Indent();
    for (const BuildInput& b : in.inputs) {
        ImGui::PushID(b.key.c_str());
        bool optional = !b.auto_text.Empty();
        // a required input still empty: the game's disc looked for in the discs folder, once a session
        static std::set<std::string> looked, found_there;
        std::string id = gm.entry.id + "/" + b.key;
        if (!optional && gm.store.GetOwn(b.key).empty() && looked.insert(id).second) {
            if (std::string disc = FindDisc(gm, b); !disc.empty()) {
                gm.store.SetOwn(b.key, disc);
                gm.store.Save();
                found_there.insert(id);
            }
        }
        std::string v = gm.store.GetOwn(b.key);
        inputs_ok &= !v.empty() || optional;
        ImGui::TextUnformatted(Tx(b.label));
        if (!b.help.Empty()) Dim(Tx(b.help));
        if (v.empty() && optional) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.5f, 1));
            ImGui::TextWrapped("%s", Tx(b.auto_text));
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(v.empty() ? ImVec4(0.95f, 0.45f, 0.4f, 1) : ImVec4(0.85f, 0.85f, 0.88f, 1), "%s",
                               v.empty() ? Tr(S::NotChosen) : v.c_str());
            if (!v.empty() && found_there.count(id)) Dim(Tr(S::DiscFoundThere));
        }
        if (auto note = g_pick_note.find(id); note != g_pick_note.end() && note->second) {
            ImGui::PushStyleColor(ImGuiCol_Text, note->second == 1 ? ImVec4(0.95f, 0.45f, 0.4f, 1) : ImVec4(0.95f, 0.8f, 0.35f, 1));
            ImGui::TextWrapped("%s", Tr(note->second == 1 ? S::PickedPortItself : S::PickedPortsFolder));
            ImGui::PopStyleColor();
        }
        if (b.folder) {
            if (ImGui::SmallButton(Tr(S::PickFolderBtn))) PickBuildInput(gm, b, false);
            if (b.file) ImGui::SameLine();
        }
        if (b.file && ImGui::SmallButton(Tr(S::PickFileBtn))) PickBuildInput(gm, b, true);
        // an optional input picked: back to the generator's own
        if (optional && !v.empty()) {
            SameLineIfFits(Tr(S::Clear));
            if (ImGui::SmallButton(Tr(S::Clear))) gm.store.SetOwn(b.key, "");
        }
        // the official pages, and the file fetched for the player from its official server
        for (auto& [label, url] : b.links) {
            SameLineIfFits(Tx(label));
            if (ImGui::SmallButton(Tx(label))) SDL_OpenURL(url.c_str());
        }
        if (!b.download_url.empty()) {
            InputFetch& f = FetchOf(gm, b);
            std::string error;
            uint64_t done = 0;
            {
                std::lock_guard lock(f.m);
                if (!f.ready.empty()) {
                    gm.store.SetOwn(b.key, f.ready);
                    f.ready.clear();
                }
                error = f.error;
                done = f.done;
            }
            if (f.busy) {
                char pct[64];
                std::snprintf(pct, sizeof(pct), "%.0f / %.0f MB", double(done) / 1048576.0, double(b.download_size) / 1048576.0);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent(0.8f));
                ImGui::ProgressBar(b.download_size ? float(double(done) / double(b.download_size)) : -float(ImGui::GetTime()),
                                   ImVec2(320 * g.scale, 0), pct);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr(S::Cancel))) f.cancel = true;
            } else {
                SameLineIfFits(Tx(b.download_label));
                ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.6f));
                if (ImGui::SmallButton(Tx(b.download_label))) StartInputFetch(gm, b);
                ImGui::PopStyleColor();
                if (!error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", error.c_str());
            }
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
    ImGui::BeginDisabled(!tools_ok || !inputs_ok || g.proc != nullptr);
    bool press = test_build && tools_ok && inputs_ok && g.proc == nullptr;
    if (press) test_build = false;
    if (ImGui::Button(Tr(S::BuildStart), ImVec2(320 * g.scale, 0)) || press) {
        // a made game whose copy only moved: nothing to redo, the generator just links the copy again
        BuildOrUnpack(gm, made && !CopyMissing(gm));
        gm.build_form = false;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (!gm.unpack_error.empty()) {
        char b[600];
        std::snprintf(b, sizeof(b), Tr(S::UnpackFailed), gm.unpack_error.c_str());
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", b);
    }
    if (bs.stage == GameBuild::Stage::Failed) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", Tr(S::BuildFailedTitle));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.82f, 1));
        ImGui::TextWrapped("%s", bs.error.c_str());
        ImGui::PopStyleColor();
        // tests: LAUNCHER_CARD_REPORT=1 presses "Report to the launcher" once
        static bool test_card = SDL_getenv("LAUNCHER_CARD_REPORT") != nullptr;
        bool press_report = test_card;
        test_card = false;
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
        if (ImGui::Button(Tr(S::ReportToLauncher)) || press_report)
            OpenReport(gm, 5, "A geração parou (código " + std::to_string(bs.code) + "):\n" + bs.error + "\n\nO que eu fiz antes: ");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::SendToGitHub))) {
            std::string what = "A geração parou (código " + std::to_string(bs.code) + "):\n\n```\n" + bs.error + "\n```";
            OpenIssue(gm, "Geração", what);
        }
    }
}

void ReportPage();
void PerfPage();
void SettingsPerf();
void Notices();
void PeriodicChecks();

void HomePage() {
    Game& gm = G();
    ImGui::PushFont(g.title, 64);
    ImGui::TextUnformatted(Tx(gm.prof.title));
    ImGui::PopFont();
    if (!gm.prof.subtitle.Empty()) Dim(Tx(gm.prof.subtitle));
    ImGui::Dummy(ImVec2(0, 18 * g.scale));
    if (gm.crashed) CrashCard(gm);

    fs::path game = FindGame(gm);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g.scale, 14 * g.scale));
    Card("status", 0, [] {
        Game& gm = G();
        if (!Playable(gm, g.view)) {
            char head[96];
            std::snprintf(head, sizeof(head), Tr(S::OtherPlatform), PlatformName(g.view));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor(gm.StateOn(g.view, g.here))), "●");
            ImGui::SameLine();
            ImGui::Text("%s — %s", head, StateLabel(gm, g.view).c_str());
            if (g.view != g.here && gm.StateOn(g.view, g.here) == "ok") Dim(Tr(S::OtherPlatformHelp));
            return;
        }
        if (!PortCard(gm)) return;
        if (gm.prof.install.kind == "build") {
            BuildCard(gm);
            return;
        }
        fs::path game = FindGame(gm);
        TakeInstallPick();
        Installer::Status is = gm.installer.Get();
        if (gm.installer.Busy()) {
            const char* what = is.stage == Installer::Stage::Extracting ? Tr(S::Unpacking)
                               : is.stage == Installer::Stage::Checking ? Tr(S::CheckingVersion)
                                                                        : Tr(S::ReadingArchive);
            ImGui::TextColored(Accent(1.3f), "● %s", what);
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%.0f%%", is.progress * 100);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent(0.8f));
            ImGui::ProgressBar(is.stage == Installer::Stage::Extracting ? is.progress : -float(ImGui::GetTime()), ImVec2(-1, 0),
                               is.stage == Installer::Stage::Extracting ? pct : "");
            ImGui::PopStyleColor();
            if (ImGui::Button(Tr(S::Cancel))) gm.installer.Cancel();
        } else if (!game.empty()) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "● %s", Tr(S::GameFound));
            ImGui::TextDisabled("%s", U8(game).c_str());
            if (is.stage == Installer::Stage::Done) {
                if (is.version_ok) {
                    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", Tr(S::VersionOk));
                } else {
                    char b[512];
                    std::snprintf(b, sizeof(b), Tr(S::VersionOther), Tx(gm.prof.install.version));
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("wip")), "%s", b);
                }
                // the downloaded archive, once its game is in place, only takes room
                std::error_code ec;
                if (is.from_archive && fs::exists(is.archive, ec)) {
                    if (gm.confirm_delete_archive) {
                        char b[512];
                        std::snprintf(b, sizeof(b), Tr(S::DeleteArchiveConfirm), U8(is.archive.filename()).c_str());
                        ImGui::TextWrapped("%s", b);
                        if (ImGui::Button(Tr(S::Yes))) {
                            fs::remove(is.archive, ec);
                            Toast(ec ? ec.message() : std::string(Tr(S::ArchiveDeleted)));
                            gm.confirm_delete_archive = false;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(Tr(S::No))) gm.confirm_delete_archive = false;
                    } else {
                        char b[128];
                        std::snprintf(b, sizeof(b), Tr(S::DeleteArchive), double(is.archive_bytes) / (1u << 30));
                        if (ImGui::SmallButton(b)) gm.confirm_delete_archive = true;
                    }
                }
            }
            if (Playable(gm, g.here) && ImGui::SmallButton(Tr(S::ChangeGame)))
                PickInstall(gm.prof.install.files.empty() ? Pick::Download : Pick::File);
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "● %s", Tr(S::GameNotInstalled));
            bool one_file = !gm.prof.install.files.empty();
            ImGui::TextWrapped("%s", one_file && !gm.prof.install.help.Empty() ? Tx(gm.prof.install.help) : Tr(S::InstallHelp));
            if (!gm.prof.install.version.Empty()) {
                ImGui::TextDisabled("%s:", Tr(S::NeededVersion));
                ImGui::SameLine();
                ImGui::TextWrapped("%s", Tx(gm.prof.install.version));
            }
            ImGui::Spacing();
            // two ways: the launcher does it all, or the player unpacks and points
            float gap = 12 * g.scale, w = (ImGui::GetContentRegionAvail().x - gap) / 2, h = 96 * g.scale;
            auto way = [&](const char* id, const char* title, const char* text, bool lit) {
                ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_Button, lit ? Accent(0.7f) : ImVec4(0.16f, 0.16f, 0.19f, 1));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lit ? Accent(0.85f) : ImVec4(0.21f, 0.21f, 0.25f, 1));
                bool pressed = ImGui::Button(id, ImVec2(w, h));
                ImGui::PopStyleColor(2);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                float pad = 14 * g.scale;
                ImGui::PushFont(g.bold, 21);
                dl->AddText(ImVec2(at.x + pad, at.y + pad), IM_COL32_WHITE, title);
                ImGui::PopFont();
                ImGui::PushFont(g.body, 16);
                dl->AddText(nullptr, 0, ImVec2(at.x + pad, at.y + pad + 30 * g.scale), IM_COL32(225, 225, 230, 255), text, nullptr,
                            w - pad * 2);
                ImGui::PopFont();
                return pressed;
            };
            if (one_file) {
                // one file to pick: the launcher copies and checks it
                w = ImGui::GetContentRegionAvail().x;
                if (way("##file", Tr(S::InstallFile), Tr(S::InstallFileText), true)) PickInstall(Pick::File);
            } else {
                if (way("##auto", Tr(S::InstallAuto), Tr(S::InstallAutoText), true)) PickInstall(Pick::Download);
                ImGui::SameLine(0, gap);
                if (way("##manual", Tr(S::InstallManual), Tr(S::InstallManualText), gm.manual_open)) gm.manual_open = !gm.manual_open;
                if (gm.manual_open) {
                    ImGui::Spacing();
                    ImGui::TextUnformatted(Tr(S::ManualSteps));
                    ImGui::Indent();
                    ImGui::TextWrapped("%s", Tr(S::ManualStep1));
                    ImGui::TextWrapped("%s", Tr(S::ManualStep2));
                    ImGui::TextWrapped("%s", Tr(S::ManualStep3));
                    ImGui::Unindent();
                    if (ImGui::Button(Tr(S::PickImage))) PickInstall(Pick::Image);
                    ImGui::SameLine();
                    if (ImGui::Button(Tr(S::HaveFolder))) PickInstall(Pick::Folder);
                }
            }
            if (is.stage == Installer::Stage::Failed)
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s: %s", Tr(S::InstallFailed), is.error.c_str());
        }
        if (gm.last_exit != INT_MIN) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s: %.0f %s, %s %d", Tr(S::LastExit), gm.last_minutes, Tr(S::Minutes), Tr(S::ExitCode),
                                gm.last_exit);
        }
    });
    ImGui::PopStyleVar();

    if (Playable(gm, g.view) && !gm.prof.presets.empty()) {
        ImGui::Dummy(ImVec2(0, 10 * g.scale));
        ImGui::PushFont(g.bold, 21);
        ImGui::TextUnformatted(Tr(S::PresetsTitle));
        ImGui::PopFont();
        // one card a preset, the one the settings match lit
        size_t n = gm.prof.presets.size();
        float gap = 14 * g.scale, w = (ImGui::GetContentRegionAvail().x - gap * float(n - 1)) / float(n);
        // the "active" badge beside the name, or at the card's foot when a name leaves it no room (narrow cards)
        float pad = 16 * g.scale, h = 128 * g.scale;
        ImGui::PushFont(g.bold, 15);
        ImVec2 badge = ImGui::CalcTextSize(Tr(S::ActivePreset));
        ImGui::PopFont();
        badge = ImVec2(badge.x + 16 * g.scale, badge.y + 6 * g.scale);
        bool badge_below = false;
        ImGui::PushFont(g.bold, 22);
        for (const Preset& p : gm.prof.presets)
            badge_below |= pad + ImGui::CalcTextSize(Tx(p.label)).x + 8 * g.scale + badge.x + pad > w;
        ImGui::PopFont();
        // every card as tall as the longest help wrapped to a card's width: four of them leave each one narrow
        ImGui::PushFont(g.body, 16);
        for (const Preset& p : gm.prof.presets)
            h = std::max(h, pad + 34 * g.scale + ImGui::CalcTextSize(Tx(p.help), nullptr, false, w - pad * 2).y + pad +
                                (badge_below ? badge.y + 8 * g.scale : 0));
        ImGui::PopFont();
        bool any = false;
        for (size_t i = 0; i < n; i++) {
            const Preset& p = gm.prof.presets[i];
            bool active = true;
            for (auto& [k, v] : p.values)
                if (!SameValue(KeyValue(k, v), v)) active = false;
            any |= active;
            ImGui::PushID(int(i));
            if (i) ImGui::SameLine(0, gap);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? Accent(0.45f, 0.9f) : ImVec4(0.11f, 0.11f, 0.13f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? Accent(0.6f, 0.95f) : ImVec4(0.17f, 0.17f, 0.2f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Accent(0.8f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * g.scale);
            ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##card", ImVec2(w, h))) {
                for (auto& [k, v] : p.values) SetKey(k, v);
                Toast(Tx(p.label));
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            // the card's words over the button
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(at, ImVec2(at.x + w, at.y + h), true);
            ImGui::PushFont(g.bold, 22);
            dl->AddText(ImVec2(at.x + pad, at.y + pad), IM_COL32(255, 255, 255, 255), Tx(p.label));
            ImGui::PopFont();
            if (active) {
                ImGui::PushFont(g.bold, 15);
                ImVec2 b0(at.x + w - pad - badge.x, badge_below ? at.y + h - pad - badge.y : at.y + pad);
                dl->AddRectFilled(b0, ImVec2(b0.x + badge.x, b0.y + badge.y), ImGui::GetColorU32(Accent(1.1f)), 8 * g.scale);
                dl->AddText(ImVec2(b0.x + 8 * g.scale, b0.y + 3 * g.scale), IM_COL32(255, 255, 255, 255), Tr(S::ActivePreset));
                ImGui::PopFont();
            }
            ImGui::PushFont(g.body, 16);
            dl->AddText(nullptr, 0, ImVec2(at.x + pad, at.y + pad + 34 * g.scale), IM_COL32(205, 205, 212, 255), Tx(p.help),
                        nullptr, w - pad * 2);
            ImGui::PopFont();
            dl->PopClipRect();
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("%s", Tr(S::Custom));
    }

    // PLAY: the page's first focus
    ImGui::Dummy(ImVec2(0, 14 * g.scale));
    ImGui::PushFont(g.bold, 34);
    ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Accent(1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Accent(1.15f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * g.scale);
    bool playing = g.proc != nullptr;
    bool can = Playable(gm, g.view) && PortPresent(gm);
    ImGui::BeginDisabled(!can || game.empty() || playing);
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    std::string state = StateLabel(gm, g.view);
    if ((state.empty() || gm.StateOn(g.view, g.here) == "beta") && !PortPresent(gm)) state = Tr(S::PortMissing);
    const char* label = !can ? state.c_str() : playing ? Tr(S::Playing) : Tr(S::Play);
    // a port made on this PC whose code changed since the game was made: Play asks first (the old executable lacks the
    // fixes, and a player who updated the port and pressed Play reported a bug the update had already fixed -- GoW,
    // report 26). The page's "make it again" line stays as it was.
    bool stale = false;
    if (gm.prof.install.kind == "build" && NotesOnlyUpdate(gm) != 2) {
        std::error_code ec;
        const std::string port = InstalledPortVersion(gm.entry.dir), built = gm.store.GetOwn("built_version");
        stale = fs::is_regular_file(gm.entry.dir / FromU8(gm.prof.install.done), ec) && !port.empty() && !built.empty() &&
                built != port && gm.build.Get().stage != GameBuild::Stage::Running;
    }
    // tests: LAUNCHER_PLAY_PRESS=1 presses Play once, as the button (LAUNCHER_AUTOPLAY launches without asking)
    static bool test_press = SDL_getenv("LAUNCHER_PLAY_PRESS") != nullptr;
    bool press = test_press && can && !playing;
    if (press) test_press = false;
    if (ImGui::Button(label, ImVec2(std::min(ImGui::GetContentRegionAvail().x, 420 * g.scale), 84 * g.scale)) || press) {
        if (stale) ImGui::OpenPopup("stale_build");
        else Launch(g.cur);
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetMainViewport()->Size.x - 40 * g.scale, 720 * g.scale), 0));
    if (ImGui::BeginPopupModal("stale_build", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(g.bold, 24);
        ImGui::TextUnformatted(Tr(S::StaleTitle));
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Tr(S::StaleText));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(Tr(S::StaleBuildNow))) {
            if (InputsSet(gm)) BuildOrUnpack(gm, true);
            else { gm.build_form = true; gm.page = 0; }  // the folders first, as the page's own button
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::StalePlayOld))) {
            ImGui::CloseCurrentPopup();
            Launch(g.cur);
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::Cancel)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (playing) ImGui::TextDisabled("%s", Tr(S::Running));
    if (g.state.Bool("close_combo", true) && g.state.Str("on_play", "minimize") != "close") Dim(Tr(S::CloseHint));
    ImGui::Spacing();
    if (ImGui::SmallButton(Tr(S::DesktopShortcut))) MakeShortcut(gm);
}

void OptionsPage(const Page& p, bool heading = true) {
    if (heading) Heading(Tx(p.label));
    bool first = true;
    for (const Option& o : G().prof.options) {
        if (o.page != p.id) continue;
        if (first && g.focus_page) ImGui::SetKeyboardFocusHere();
        first = false;
        OptionRow(o);
        ImGui::Spacing();
    }
    // controllers plugged in, on the page with the button map
    for (const Option& o : G().prof.options)
        if (o.page == p.id && o.type == OptType::ButtonMap) {
            ImGui::Separator();
            ImGui::TextUnformatted(Tr(S::Controllers));
            int n = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&n);
            if (!n) ImGui::TextDisabled("%s", Tr(S::NoControllers));
            for (int i = 0; i < n; i++) {
                const char* name = SDL_GetGamepadNameForID(ids[i]);
                ImGui::BulletText("%s", name ? name : "?");
            }
            SDL_free(ids);
            break;
        }
}

void AdvancedPage() {
    Heading(Tr(S::Advanced));
    std::vector<int> pages = AdvancedPages();
    if (pages.empty()) return;
    Game& gm = G();
    gm.tab = std::clamp(gm.tab, 0, int(pages.size()) - 1);
    if (ImGui::BeginTabBar("adv")) {
        for (size_t i = 0; i < pages.size(); i++) {
            ImGuiTabItemFlags f = g.focus_tab && int(i) == gm.tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(Tx(gm.prof.pages[size_t(pages[i])].label), nullptr, f)) {
                if (!g.focus_tab) gm.tab = int(i);
                ImGui::Spacing();
                OptionsPage(gm.prof.pages[size_t(pages[i])], false);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    g.focus_tab = false;
}

std::string Bytes(uint64_t b) {
    char s[32];
    if (b >= 1048576) std::snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
    else std::snprintf(s, sizeof(s), "%.0f KB", b / 1024.0);
    return s;
}

void SavesPageUi() {
    Game& gm = G();
    Heading(Tr(S::Saves));
    Dim(Tr(S::SavesHelp));
    ImGui::Spacing();
    for (const std::string& s : gm.prof.saves) {
        fs::path d = gm.entry.dir / FromU8(s);
        uint64_t b = FolderBytes(d);
        ImGui::PushID(s.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s: %s (%s)", Tr(S::SaveFolder), s.c_str(), b ? Bytes(b).c_str() : Tr(S::Empty));
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::OpenFolder))) OpenPath(d);
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button(Tr(S::BackupNow))) {
        std::string err;
        if (MakeBackup(gm.entry.dir, gm.prof, "", err)) Toast(Tr(S::BackupDone));
        else Toast(err == "no saves" ? Tr(S::Empty) : err);
        gm.backups_stale = true;
    }
    ImGui::SameLine();
    bool autob = gm.store.GetOwnBool("auto_backup", true);
    if (ImGui::Checkbox(Tr(S::BackupAuto), &autob)) {
        gm.store.SetOwnBool("auto_backup", autob);
        gm.store.Save();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(S::Backups));
    ImGui::SameLine();
    if (ImGui::SmallButton(Tr(S::OpenFolder))) OpenPath(gm.entry.dir / "backups");
    if (gm.backups_stale) {
        gm.backups = ListBackups(gm.entry.dir);
        gm.backups_stale = false;
    }
    if (gm.backups.empty()) ImGui::TextDisabled("%s", Tr(S::NoBackups));
    for (size_t i = 0; i < gm.backups.size(); i++) {
        const Backup& b = gm.backups[i];
        ImGui::PushID(int(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  ·  %s", b.name.c_str(), Bytes(b.bytes).c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.7f);
        if (gm.confirm_restore == int(i)) {
            if (ImGui::Button(Tr(S::Yes))) {
                std::string err;
                if (RestoreBackup(gm.entry.dir, gm.prof, b, err)) Toast(Tr(S::RestoreDone));
                else Toast(err);
                gm.confirm_restore = -1;
                gm.backups_stale = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr(S::No))) gm.confirm_restore = -1;
            ImGui::TextColored(Accent(1.3f), "%s", Tr(S::RestoreConfirm));
        } else if (ImGui::Button(Tr(S::Restore))) {
            gm.confirm_restore = int(i);
        }
        ImGui::PopID();
    }
}

void AboutPageUi() {
    Game& gm = G();
    Heading(Tr(S::About));
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button(Tr(S::OpenLogs))) OpenPath(gm.entry.dir / FromU8(gm.prof.logs));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(S::Credits));
    for (auto& [name, what] : gm.prof.credits) ImGui::BulletText("%s — %s", Tx(name), Tx(what));
    ImGui::Spacing();
    if (!gm.prof.legal.Empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(Tr(S::Legal));
        Dim(Tx(gm.prof.legal));
    }
}

// ---------------------------------------------------------------------------------------------- the hub's pages

// the hub's sidebar: all games, each library that has a game, then the launcher's settings
struct HubEntry {
    int library = -1;  // index in hub.libraries; -1 all games
    bool settings = false;
    bool requests = false;
    bool tutorial = false;
};

std::vector<HubEntry> HubEntries() {
    std::vector<HubEntry> e{{-1, false, false, false}};
    for (size_t i = 0; i < g.hub.libraries.size(); i++) e.push_back({int(i), false, false, false});
    e.push_back({-1, false, false, true});
    e.push_back({-1, false, true, false});
    e.push_back({-1, true, false, false});
    return e;
}

bool Listed(const Game& gm, int library) {
    return library < 0 || gm.entry.library == g.hub.libraries[size_t(library)].id;
}

int GamesIn(int library) {
    int n = 0;
    for (auto& gp : g.games)
        if (Listed(*gp, library)) n++;
    return n;
}

void OpenGame(int i) {
    g.cur = i;
    Platform other = g.here == Platform::Windows ? Platform::Linux : Platform::Windows;
    g.view = G().StateOn(g.here, g.here).empty() && !G().StateOn(other, g.here).empty() ? other : g.here;
    g.screen = Screen::Game;
    G().page = 0;
    g.focus_page = true;
    g.art_t0 = SDL_GetTicks();
    g.rep_msg.clear();
    ApplyTheme();
    PlayTheme();
}

void BackToHub() {
    if (g.cur >= 0 && G().store.Dirty()) G().store.Save();
    g.screen = Screen::Hub;
    g.focus_page = true;
    g.art_t0 = SDL_GetTicks();
    g.music.Stop();
    ApplyTheme();
}

// a game's card: its art over the whole card, the title on a dark band at the foot
bool GameCard(Game& gm, ImVec2 size, bool focus) {
    ImGui::PushID(gm.entry.id.c_str());
    ImVec2 at = ImGui::GetCursorScreenPos();
    if (focus) ImGui::SetKeyboardFocusHere();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.11f, 0.11f, 0.13f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.16f, 0.19f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.24f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14 * g.scale);
    bool pressed = ImGui::Button("##game", size);
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = 14 * g.scale;
    ImVec2 end(at.x + size.x, at.y + size.y);
    if (!gm.art.empty()) {
        auto [tex, ts] = gm.art[0];
        ImVec2 uv0, uv1;
        CoverUv(ts, size, uv0, uv1);
        dl->AddImageRounded(Tex(tex), at, end, uv0, uv1, IM_COL32(255, 255, 255, hot ? 255 : 215), r);
    } else {
        auto a = gm.prof.accent;
        ImU32 c0 = ImGui::GetColorU32(AccentOf(a, 0.55f)), c1 = ImGui::GetColorU32(AccentOf(a, 0.18f));
        dl->AddRectFilledMultiColor(at, end, c0, c1, c1, c0);
    }
    // the foot band and the words
    float band = 74 * g.scale;
    dl->AddRectFilledMultiColor(ImVec2(at.x, end.y - band * 1.6f), end, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                IM_COL32(6, 6, 8, 235), IM_COL32(6, 6, 8, 235));
    float pad = 16 * g.scale;
    ImGui::PushFont(g.bold, 24);
    dl->AddText(ImVec2(at.x + pad, end.y - band + 6 * g.scale), IM_COL32(255, 255, 255, 255), Tx(gm.prof.title));
    ImGui::PopFont();
    ImGui::PushFont(g.body, 16);
    const Library* lib = FindLibrary(gm.entry.library);
    std::string sub = lib ? lib->name.Get(g.lang) : gm.entry.library;
    dl->AddText(ImVec2(at.x + pad, end.y - band + 38 * g.scale), IM_COL32(200, 200, 208, 255), sub.c_str());
    std::string state = StateLabel(gm, g.here);
    if (!state.empty()) {
        const char* w = state.c_str();
        ImVec2 ts = ImGui::CalcTextSize(w);
        ImVec2 b0(at.x + pad, at.y + pad);
        dl->AddRectFilled(b0, ImVec2(b0.x + ts.x + 16 * g.scale, b0.y + ts.y + 8 * g.scale), ImGui::GetColorU32(Accent(0.8f)),
                          8 * g.scale);
        dl->AddText(ImVec2(b0.x + 8 * g.scale, b0.y + 4 * g.scale), IM_COL32(255, 255, 255, 255), w);
    }
    ImGui::PopFont();
    ImGui::PushFont(g.bold, 14);
    float x = end.x - pad;
    for (Platform p : {Platform::Linux, Platform::Windows}) {
        std::string st = gm.StateOn(p, g.here);
        if (st.empty()) continue;
        const char* name = PlatformName(p);
        ImVec2 ts = ImGui::CalcTextSize(name);
        float dot = 4 * g.scale, w = ts.x + dot * 2 + 22 * g.scale, h = ts.y + 8 * g.scale;
        ImVec2 b0(x - w, end.y - pad - h);
        dl->AddRectFilled(b0, ImVec2(x, b0.y + h), IM_COL32(0, 0, 0, 170), h / 2);
        dl->AddCircleFilled(ImVec2(b0.x + 9 * g.scale + dot, b0.y + h / 2), dot, StateColor(st));
        dl->AddText(ImVec2(b0.x + 13 * g.scale + dot * 2, b0.y + 4 * g.scale), IM_COL32(230, 230, 236, 255), name);
        x = b0.x - 6 * g.scale;
    }
    ImGui::PopFont();
    // the library's colour along the top when lit
    if (hot) dl->AddRect(at, end, ImGui::GetColorU32(Accent(1.3f)), r, 0, 3 * g.scale);
    ImGui::PopID();
    return pressed;
}

void RequestsPage();

void UpdateActions(const UpdateCheck& u);

// installed here: the port on this PC with its game ready (made, for a game built on the PC, or found); looked up every
// few seconds, the search for a game's folder being too slow for every frame
bool InstalledHere(int i) {
    static std::vector<char> cache;
    static Uint64 at = 0;
    Uint64 now = SDL_GetTicks();
    if (cache.size() != g.games.size() || now - at > 3000) {
        cache.assign(g.games.size(), 0);
        for (size_t k = 0; k < g.games.size(); k++) {
            Game& gm = *g.games[k];
            cache[k] = gm.ok && PortPresent(gm) && !FindGame(gm).empty();
        }
        at = now;
    }
    return i >= 0 && size_t(i) < cache.size() && cache[size_t(i)];
}

// the games shown, in a library page and in the search alike, kept between sessions: everything, only the ones
// installed here, only the ports that play here, or only those still in development (the search's catalog, games
// without a port, shows only under everything); X on the controller goes round them
constexpr int kFilterOrder[] = {0, 3, 1, 2};  // as the buttons show them; the values are search_filter's
constexpr S kFilterNames[] = {S::FilterAll, S::FilterPlayable, S::FilterWip, S::FilterInstalled};

bool PassesFilter(int i) {
    if (g.search_filter == 0) return true;
    if (g.search_filter == 3) return InstalledHere(i);
    std::string st = g.games[size_t(i)]->StateOn(g.here, g.here);
    return g.search_filter == 1 ? st == "ok" : st == "wip" || st == "beta";
}

void SetFilter(int f) {
    g.search_filter = f;
    g.state.Set("library_filter", int64_t(f));
}

void NextFilter() {
    size_t at = 0;
    while (at < std::size(kFilterOrder) && kFilterOrder[at] != g.search_filter) at++;
    SetFilter(kFilterOrder[(at + 1) % std::size(kFilterOrder)]);
}

// the buttons, then how many games the filter leaves, so a filtered library never looks emptied
void FilterButtons(size_t shown) {
    for (size_t k = 0; k < std::size(kFilterOrder); k++) {
        int f = kFilterOrder[k];
        if (k) ImGui::SameLine();
        bool on = g.search_filter == f;
        ImGui::PushStyleColor(ImGuiCol_Button, on ? Accent(0.75f) : ImVec4(0.16f, 0.16f, 0.19f, 1));
        if (ImGui::Button(Tr(kFilterNames[f]))) SetFilter(f);
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 16 * g.scale);
    char b[64];
    if (shown == 1)
        std::snprintf(b, sizeof(b), "%s", Tr(S::OneGame));
    else
        std::snprintf(b, sizeof(b), Tr(S::GamesCount), int(shown));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", b);
    ImGui::Spacing();
}

void LibraryPage(int library) {
    const char* name = library < 0 ? Tr(S::AllGames) : Tx(g.hub.libraries[size_t(library)].name);
    Heading(name);
    UpdateCheck u = g.updater.Get();
    if (u.state == UpdateCheck::State::Newer || u.state == UpdateCheck::State::Downloading ||
        u.state == UpdateCheck::State::Ready) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * g.scale, 12 * g.scale));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Accent(0.3f, 0.6f));
        ImGui::BeginChild("update", ImVec2(-1, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        char b[160];
        std::snprintf(b, sizeof(b), Tr(S::LauncherNewer), u.latest.c_str());
        ImGui::TextUnformatted(u.state == UpdateCheck::State::Newer ? b
                               : u.state == UpdateCheck::State::Downloading ? Tr(S::DownloadingUpdate)
                                                                            : Tr(S::InstallingUpdate));
        UpdateActions(u);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::Spacing();
    }
    std::vector<int> list;
    bool any = false;
    for (size_t i = 0; i < g.games.size(); i++)
        if (Listed(*g.games[i], library)) {
            any = true;
            if (PassesFilter(int(i))) list.push_back(int(i));
        }
    if (!any) {
        Dim(Tr(S::NoGames));
        return;
    }
    FilterButtons(list.size());
    if (list.empty()) {
        Dim(Tr(S::SearchNothing));
        return;
    }
    // as many columns as fit cards of about 420 wide, 16:9
    float avail = ImGui::GetContentRegionAvail().x, gap = 20 * g.scale;
    int cols = std::max(1, int((avail + gap) / (420 * g.scale + gap)));
    float w = (avail - gap * float(cols - 1)) / float(cols), h = w * 9.0f / 16.0f;
    for (size_t k = 0; k < list.size(); k++) {
        if (k % size_t(cols)) ImGui::SameLine(0, gap);
        if (GameCard(*g.games[size_t(list[k])], ImVec2(w, h), g.focus_page && k == 0)) OpenGame(list[k]);
        if (k % size_t(cols) == size_t(cols - 1)) ImGui::Dummy(ImVec2(0, gap * 0.25f));
    }
}

// the discs folder and the games folder picked in Settings: the dialogs answer on their own thread
std::mutex g_discs_m;
std::string g_discs_pick, g_games_pick;

void GamesFolderPicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lock(g_discs_m);
    g_games_pick = files[0];
}

// a new games folder (empty: the hub's places), at once: the installed games pinned where they are first, then each
// port not installed yet (and not downloading) moved to its new folder
void SetGamesFolder(const std::string& folder) {
    if (!folder.empty() && !GoodGamesFolder(folder)) {
        Toast(Tr(S::GamesFolderBad), 7000);
        return;
    }
    PinInstalledGames();
    g.state.Set("games_folder", folder);
    for (auto& p : g.games) {
        Game& gm = *p;
        if (gm.fetcher.Busy()) continue;
        fs::path before = gm.entry.dir;
        PlaceGame(gm);
        if (gm.entry.dir != before) OpenGameFolder(gm);
    }
}

void DiscsFolderPicked(void*, const char* const* files, int) {
    if (!files || !files[0]) return;
    std::lock_guard lock(g_discs_m);
    g_discs_pick = files[0];
}

void SettingsGeneral() {
    // language
    static const char* kCodes[] = {"auto", "pt", "en", "es"};
    static const char* kNames[] = {nullptr, "Português (Brasil)", "English", "Español"};
    std::string lang = g.state.Str("language", "auto");
    RowLabel(Tr(S::Language));
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    const char* shown = lang == "auto" ? Tr(S::LanguageAuto) : lang == "pt" ? kNames[1] : lang == "es" ? kNames[3] : kNames[2];
    if (ImGui::BeginCombo("##lang", shown)) {
        for (int i = 0; i < 4; i++)
            if (ImGui::Selectable(i == 0 ? Tr(S::LanguageAuto) : kNames[i], lang == kCodes[i])) {
                g.state.Set("language", std::string(kCodes[i]));
                g.lang = i == 0 ? SystemLang() : i == 1 ? Lang::Pt : i == 2 ? Lang::En : Lang::Es;
            }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    // window
    bool full = g.state.Str("window_mode", "window") == "fullscreen";
    RowLabel(Tr(S::WindowMode));
    if (ImGui::BeginCombo("##mode", full ? Tr(S::Fullscreen) : Tr(S::Windowed))) {
        if (ImGui::Selectable(Tr(S::Windowed), !full)) {
            g.state.Set("window_mode", std::string("window"));
            SDL_SetWindowFullscreen(g.win, false);
        }
        if (ImGui::Selectable(Tr(S::Fullscreen), full)) {
            g.state.Set("window_mode", std::string("fullscreen"));
            SDL_SetWindowFullscreen(g.win, true);
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    static const std::pair<int, int> kSizes[] = {{1280, 800}, {1440, 900}, {1600, 1000}, {1920, 1080}, {1920, 1200}};
    int ww = int(g.state.Int("window_w", 1280)), wh = int(g.state.Int("window_h", 800));
    char cur[32];
    std::snprintf(cur, sizeof(cur), "%d x %d", ww, wh);
    RowLabel(Tr(S::WindowSize));
    ImGui::BeginDisabled(full);
    if (ImGui::BeginCombo("##size", cur)) {
        for (auto [sw, sh] : kSizes) {
            char b[32];
            std::snprintf(b, sizeof(b), "%d x %d", sw, sh);
            if (ImGui::Selectable(b, sw == ww && sh == wh)) {
                g.state.Set("window_w", int64_t(sw));
                g.state.Set("window_h", int64_t(sh));
                SDL_SetWindowSize(g.win, int(sw * g.content_scale), int(sh * g.content_scale));
                SDL_SetWindowPosition(g.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    int ui = int(g.state.Int("ui_scale", 100));
    RowLabel(Tr(S::UiScale));
    if (ImGui::SliderInt("##ui", &ui, 70, 160, "%d%%", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
        ui = (ui + 5) / 10 * 10;
        if (ui != g.state.Int("ui_scale", 100)) {
            g.state.Set("ui_scale", int64_t(ui));
            ApplyScale();
        }
    }
    ImGui::Spacing();
    // when a game starts
    static const char* kPlay[] = {"minimize", "keep", "close"};
    std::string on_play = g.state.Str("on_play", "minimize");
    S play_names[] = {S::OnPlayMinimize, S::OnPlayKeep, S::OnPlayClose};
    int pi = on_play == "keep" ? 1 : on_play == "close" ? 2 : 0;
    RowLabel(Tr(S::OnPlay));
    if (ImGui::BeginCombo("##play", Tr(play_names[pi]))) {
        for (int i = 0; i < 3; i++)
            if (ImGui::Selectable(Tr(play_names[i]), i == pi)) g.state.Set("on_play", std::string(kPlay[i]));
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    bool anim = g.state.Bool("bg_anim", true);
    RowLabel(Tr(S::BgAnim));
    if (ImGui::Checkbox("##anim", &anim)) g.state.Set("bg_anim", anim);
    ImGui::Spacing();
    bool music = g.state.Bool("music", true);
    RowLabel(Tr(S::ThemeMusic));
    if (ImGui::Checkbox("##music", &music)) {
        g.state.Set("music", music);
        if (!music) g.music.Stop();
    }
    ImGui::Spacing();
    int vol = int(g.state.Int("music_volume", 20));
    RowLabel(Tr(S::MusicVolume));
    ImGui::BeginDisabled(!music);
    if (ImGui::SliderInt("##mvol", &vol, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
        g.state.Set("music_volume", int64_t(vol));
        g.music.SetVolume(float(vol) / 100.0f);
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    bool combo = g.state.Bool("close_combo", true);
    RowLabel(Tr(S::CloseCombo));
    if (ImGui::Checkbox("##combo", &combo)) g.state.Set("close_combo", combo);
    ImGui::Spacing();
    bool last = g.state.Bool("start_last", false);
    RowLabel(Tr(S::StartLast));
    if (ImGui::Checkbox("##last", &last)) g.state.Set("start_last", last);
    ImGui::Spacing();
    // the discs folder (DiscsFolder): where the disc pickers open and the games' discs are found by name
    {
        std::lock_guard lock(g_discs_m);
        if (!g_discs_pick.empty()) {
            g.state.Set("discs_folder", g_discs_pick);
            g_discs_pick.clear();
        }
    }
    std::string discs = g.state.Str("discs_folder", "");
    // the buttons beside the name, the path under them (a long one would push the buttons off the page)
    RowLabel(Tr(S::DiscsFolder));
    if (ImGui::SmallButton(Tr(S::PickFolderBtn)))
        SDL_ShowOpenFolderDialog(DiscsFolderPicked, nullptr, g.win, discs.empty() ? nullptr : discs.c_str(), false);
    if (!discs.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr(S::Clear))) g.state.Set("discs_folder", std::string());
    }
    ImGui::TextWrapped("%s", discs.empty() ? Tr(S::NotChosen) : discs.c_str());
    Dim(Tr(S::DiscsFolderHelp));
    ImGui::Spacing();
    // the games folder (GamesFolder): where the ports not installed yet go, the installed ones staying where they are;
    // not while a game is made or played
    std::string games_pick;
    {
        std::lock_guard lock(g_discs_m);
        games_pick.swap(g_games_pick);
    }
    if (!games_pick.empty()) SetGamesFolder(games_pick);
    std::string games = g.state.Str("games_folder", "");
    RowLabel(Tr(S::GamesFolder));
    ImGui::BeginDisabled(g.proc != nullptr);
    if (ImGui::SmallButton(Tr(S::PickFolderBtn)))
        SDL_ShowOpenFolderDialog(GamesFolderPicked, nullptr, g.win, games.empty() ? nullptr : games.c_str(), false);
    if (!games.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr(S::Clear))) SetGamesFolder("");
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", games.empty() ? Tr(S::GamesFolderAuto) : games.c_str());
    Dim(Tr(S::GamesFolderHelp));
}

// a newer launcher: updated in place (downloaded, installed silently over this folder, reopened), or its page
void UpdateActions(const UpdateCheck& u) {
    static bool ran = false;
    if (u.state == UpdateCheck::State::Newer) {
        ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
        ImGui::BeginDisabled(u.installer.empty() || g.proc != nullptr);
        if (ImGui::Button(Tr(S::UpdateNow))) g.updater.Download();
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::WhatsNew))) SDL_OpenURL(u.url.c_str());
    } else if (u.state == UpdateCheck::State::Downloading) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.1f / %.1f MB", double(u.done) / 1048576.0, double(u.size) / 1048576.0);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent(0.8f));
        ImGui::ProgressBar(u.size ? float(double(u.done) / double(u.size)) : 0.0f, ImVec2(-1, 0), b);
        ImGui::PopStyleColor();
    } else if (u.state == UpdateCheck::State::Ready && !ran) {
        ran = true;
        std::string err;
        if (RunInstaller(u.file, g.hub_dir, err)) g.quit = true;
        else Toast(std::string(Tr(S::UpdateFailed)) + ": " + err);
    }
}

void SubHeading(const char* text);

// a line of a release's notes without Markdown's marks: [text](address) as text, no ** __ or `
std::string MdInline(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '[') {
            size_t close = s.find("](", i);
            size_t end = close == std::string::npos ? close : s.find(')', close);
            if (end != std::string::npos) {
                out += s.substr(i + 1, close - i - 1);
                i = end;
                continue;
            }
        }
        if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] == s[i]) {
            i++;
            continue;
        }
        if (s[i] == '`') continue;
        out += s[i];
    }
    return out;
}

// a release's notes: GitHub's Markdown as headings, bullets and paragraphs
void MarkdownText(const std::string& md) {
    size_t a = 0;
    while (a <= md.size()) {
        size_t b = md.find('\n', a);
        if (b == std::string::npos) b = md.size();
        std::string line = md.substr(a, b - a);
        a = b + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t lead = line.find_first_not_of(' ');
        if (lead == std::string::npos) {
            ImGui::Spacing();
            continue;
        }
        std::string text = line.substr(lead);
        if (text[0] == '#') {
            size_t level = text.find_first_not_of('#');
            if (level == std::string::npos) continue;
            ImGui::PushFont(g.bold, level <= 1 ? 22.0f : level == 2 ? 20.0f : 19.0f);
            ImGui::TextWrapped("%s", MdInline(text.substr(text.find_first_not_of(' ', level))).c_str());
            ImGui::PopFont();
        } else if (text.size() > 1 && (text[0] == '-' || text[0] == '*') && text[1] == ' ') {
            if (lead >= 2) ImGui::Indent(float(lead / 2) * 16 * g.scale);
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", MdInline(text.substr(2)).c_str());
            if (lead >= 2) ImGui::Unindent(float(lead / 2) * 16 * g.scale);
        } else {
            ImGui::TextWrapped("%s", MdInline(text).c_str());
        }
    }
}

// "2026-10-25" as 25/10/2026
std::string DateFromIso(const std::string& d) {
    return d.size() == 10 ? d.substr(8, 2) + "/" + d.substr(5, 2) + "/" + d.substr(0, 4) : d;
}

// a repository's releases, newest first, each a section with its notes; the installed one marked
void NotesBlock(const std::string& repo, const std::string& installed, bool open_newest) {
    if (repo.empty()) return;
    g.notes.Fetch(repo);
    std::vector<Release> list;
    ReleaseNotes::State st = g.notes.Get(repo, &list);
    if (st == ReleaseNotes::State::Loading || st == ReleaseNotes::State::None) {
        Dim(Tr(S::Loading));
        return;
    }
    if (list.empty()) {
        Dim(Tr(S::NotesNone));
        return;
    }
    auto bare = [](std::string v) { return !v.empty() && (v[0] == 'v' || v[0] == 'V') ? v.substr(1) : v; };
    for (size_t i = 0; i < list.size() && i < 10; i++) {
        const Release& r = list[i];
        bool mine = !installed.empty() && bare(r.tag) == bare(installed);
        std::string label = r.tag;
        if (!r.name.empty() && r.name != r.tag) label += " · " + r.name;
        label += "   " + DateFromIso(r.date);
        if (mine) label += "   ● " + std::string(Tr(S::NotesInstalled));
        ImGui::PushID((repo + "/" + r.tag).c_str());
        if (ImGui::CollapsingHeader(label.c_str(), i == 0 && open_newest ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::Indent(12 * g.scale);
            if (r.body.empty()) Dim(Tr(S::NotesNone));
            else MarkdownText(r.body);
            ImGui::Unindent(12 * g.scale);
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
}

// a port's NOVIDADES.md: "## <date> · <version>" entries, newest first, each with "### pt" / "### en" / "### es"
// sections (a body without them serves every language)
struct NewsEntry {
    std::string head, body;
};
std::vector<NewsEntry> ParseNews(const std::string& md) {
    std::vector<NewsEntry> out;
    size_t at = 0;
    while (at < md.size()) {
        size_t end = md.find('\n', at);
        if (end == std::string::npos) end = md.size();
        std::string line = md.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## ", 0) == 0) out.push_back({line.substr(3), ""});
        else if (!out.empty()) out.back().body += line + "\n";
        at = end + 1;
    }
    return out;
}
std::string NewsBody(const std::string& body, Lang lang) {
    auto section = [&](const char* code) -> std::string {
        const std::string mark = std::string("### ") + code;
        size_t at = body.find(mark);
        if (at == std::string::npos) return {};
        at = body.find('\n', at);
        if (at == std::string::npos) return {};
        size_t end = body.find("\n### ", at);
        return body.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
    };
    std::string s = section(lang == Lang::En ? "en" : lang == Lang::Es ? "es" : "pt");
    if (s.empty()) s = section("pt");
    return s.empty() ? body : s;
}

// the game's "What's new": a port that follows a branch (made of commits, no release per fix) keeps its notes for the
// players in NOVIDADES.md -- the one in the port folder is what the player has, the one on GitHub what the update
// brings; a port with releases shows their notes, as the settings do
void NewsPage(Game& gm) {
    Heading(Tr(S::News));
    Dim(Tr(S::NewsIntro));
    ImGui::Spacing();
    const std::string port = InstalledPortVersion(gm.entry.dir);
    if (gm.entry.branch.empty()) {
        NotesBlock(gm.entry.repo, port, true);
        return;
    }
    if (gm.prof.install.kind == "build") {
        NotesOnlyUpdate(gm);
        const std::string built = gm.store.GetOwn("built_version");
        if (!port.empty() && !built.empty() && built != port) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(StateColor("wip")));
            ImGui::TextWrapped("● %s", Tr(S::NewsRebuild));
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
    }
    std::string local;
    {
        std::ifstream f(gm.entry.dir / "NOVIDADES.md", std::ios::binary);
        if (f) local.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    g.notes.FetchFile(gm.entry.repo, gm.entry.branch, "NOVIDADES.md");
    std::string remote;
    ReleaseNotes::State st = g.notes.GetFile(gm.entry.repo, gm.entry.branch, "NOVIDADES.md", &remote);
    std::vector<NewsEntry> mine = ParseNews(local), all = ParseNews(remote);
    if (all.empty()) all = mine;   // offline, or the file not on GitHub: what the port folder has
    if (all.empty()) {
        Dim(st == ReleaseNotes::State::Loading ? Tr(S::Loading) : Tr(S::NewsNone));
        return;
    }
    for (size_t i = 0; i < all.size() && i < 30; i++) {
        bool have = false;
        for (const NewsEntry& n : mine) have = have || n.head == all[i].head;
        // a page with nothing to install here (a port still in development): the notes without "in your port" marks
        std::string label = all[i].head;
        if (Playable(gm, g.here)) label += std::string("   ● ") + Tr(have ? S::NewsInstalled : S::NewsInUpdate);
        ImGui::PushID(int(i));
        ImGui::PushStyleColor(ImGuiCol_Text, have || !Playable(gm, g.here) ? ImVec4(0.92f, 0.92f, 0.94f, 1) : ImGui::ColorConvertU32ToFloat4(StateColor("wip")));
        const bool open = ImGui::CollapsingHeader(label.c_str(), i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleColor();
        if (open) {
            ImGui::Indent(12 * g.scale);
            MarkdownText(NewsBody(all[i].body, g.lang));
            ImGui::Unindent(12 * g.scale);
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
}

// the ports installed and playable here, by their place in g.games
std::vector<int> InstalledPorts() {
    std::vector<int> installed;
    for (size_t i = 0; i < g.games.size(); i++) {
        Game& gm = *g.games[i];
        if (!gm.entry.repo.empty() && gm.ok && Playable(gm, g.here) && PortPresent(gm)) installed.push_back(int(i));
    }
    return installed;
}

// "Update and make all": first each installed port in turn, its newest version looked up and installed; then, one
// after the other, each game made on this PC made again when its port's code is newer than the one it was made with
// (or made for the first time, its folders already picked). A game being played is waited for; a port whose update
// failed is not made.
struct BulkItem {
    int game = -1;
    bool updated = false, built = false;
    bool current = false;       // already made with the port's code
    bool needs_inputs = false;  // its folders not picked yet: made from its page
    std::string error;
};
struct BulkRun {
    std::vector<BulkItem> items;
    size_t at = 0;
    int pass = 0;   // 0: the ports updated, 1: the games made
    int phase = 0;  // the current item's: 0 to start, 1 its version being looked up, 2 downloaded, 3 made
    bool running = false, cancel = false, finished = false, waiting = false;
} g_bulk;

bool BulkFailed() {
    for (const BulkItem& it : g_bulk.items)
        if (!it.error.empty()) return true;
    return false;
}

void StartBulk() {
    g_bulk = {};
    for (int i : InstalledPorts()) {
        BulkItem it;
        it.game = i;
        g_bulk.items.push_back(it);
    }
    g_bulk.running = !g_bulk.items.empty();
}

void CancelBulk() {
    if (!g_bulk.running || g_bulk.at >= g_bulk.items.size()) return;
    g_bulk.cancel = true;
    Game& gm = *g.games[size_t(g_bulk.items[g_bulk.at].game)];
    if (g_bulk.pass == 0 && g_bulk.phase == 2) gm.fetcher.Cancel();
    if (g_bulk.pass == 1 && g_bulk.phase == 3) {
        if (gm.installer.Busy()) gm.installer.Cancel();
        if (gm.build.Busy()) gm.build.Cancel();
    }
}

// one look per frame, from the main loop: the settings page need not be open
void BulkTick() {
    // tests: LAUNCHER_UPDATE_ALL=1 presses the button once
    if (static bool test = SDL_getenv("LAUNCHER_UPDATE_ALL") != nullptr; test) {
        test = false;
        StartBulk();
    }
    BulkRun& b = g_bulk;
    if (!b.running) return;
    auto next = [&] {
        b.phase = 0;
        if (++b.at >= b.items.size()) b.at = 0, b.pass++;
    };
    auto stop = [&] {
        b.running = false;
        b.finished = true;
        b.waiting = false;
    };
    if (b.pass > 1) {
        stop();
        Toast(Tr(BulkFailed() ? S::UpdateAllDoneErrors : S::UpdateAllDone), 6000);
        return;
    }
    BulkItem& it = b.items[b.at];
    Game& gm = *g.games[size_t(it.game)];
    // nothing new starts while a game is open
    b.waiting = b.phase <= 1 && g.proc != nullptr;
    if (b.phase == 0 && b.cancel) {
        stop();
        return;
    }
    if (b.pass == 0) {
        if (b.phase == 0) {
            if (g.proc || gm.fetcher.Busy()) return;  // also the game's own page downloading it
            gm.port_checked = true;
            gm.fetcher.Check(gm.entry.Source(), InstalledPortVersion(gm.entry.dir));
            b.phase = 1;
        } else if (b.phase == 1) {
            if (gm.fetcher.Busy()) return;
            PortFetcher::Status ps = gm.fetcher.Get();
            std::string latest;
            if (ps.stage == PortFetcher::Stage::Failed) {
                it.error = ps.error;
                next();
            } else if (b.cancel || !PortHasUpdate(gm, latest)) {
                next();
            } else if (!g.proc) {
                gm.fetcher.Install(gm.entry.Source(), gm.entry.dir);
                b.phase = 2;
            }
        } else {
            if (gm.fetcher.Busy()) return;
            PortFetcher::Status ps = gm.fetcher.Get();
            if (ps.stage == PortFetcher::Stage::Done) it.updated = true;
            else it.error = b.cancel ? Tr(S::UpdateAllStopped) : ps.error.empty() ? Tr(S::PortFailed) : ps.error;
            next();
        }
        return;
    }
    if (gm.prof.install.kind != "build" || !it.error.empty()) {
        next();
        return;
    }
    if (b.phase == 0) {
        if (g.proc || gm.build.Busy() || !gm.unpack_key.empty()) return;  // also one started from the game's page
        const InstallSpec& in = gm.prof.install;
        std::error_code ec;
        bool made = fs::is_regular_file(gm.entry.dir / FromU8(in.done), ec);
        // an update of notes only needs no making: wait for GitHub's answer (one call), then go on
        if (made && NotesOnlyUpdate(gm) == 1) return;
        std::string port = InstalledPortVersion(gm.entry.dir), built = gm.store.GetOwn("built_version");
        if (made && built.empty()) built = port;  // as the game's page takes it
        bool inputs_set = InputsSet(gm);
        if (made && built == port) {
            it.current = true;
        } else if (!inputs_set) {
            it.needs_inputs = true;
        } else {
            gm.unpack_error.clear();
            gm.build_form = false;
            BuildOrUnpack(gm, made);
            b.phase = 3;
            return;
        }
        next();
        return;
    }
    // being made: an archive or disc image unpacked first, then the generator
    if (UnpackStep(gm) || !gm.unpack_key.empty() || gm.build.Busy()) return;
    GameBuild::Status bs = gm.build.Get();
    if (b.cancel) {
        it.error = Tr(S::UpdateAllStopped);
    } else if (!gm.unpack_error.empty()) {
        it.error = gm.unpack_error;
    } else if (bs.stage == GameBuild::Stage::Done) {
        NoteBuild(gm);
        it.built = true;
    } else {
        // why, from the log's end: its last line that is not part of PowerShell's error record ("+ CategoryInfo",
        // "+ FullyQualifiedErrorId", "No C:\...:123 caractere:5" / "At ...", the "~~~" under the command)
        std::vector<std::string> lines;
        for (size_t at = 0; at <= bs.error.size();) {
            size_t nl = bs.error.find('\n', at);
            if (nl == std::string::npos) nl = bs.error.size();
            std::string l = bs.error.substr(at, nl - at);
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            size_t s = l.find_first_not_of(' ');
            if (s != std::string::npos) lines.push_back(l.substr(s));
            at = nl + 1;
        }
        for (auto l = lines.rbegin(); l != lines.rend() && it.error.empty(); ++l)
            if ((*l)[0] != '+' && (*l)[0] != '~' && l->rfind("No ", 0) != 0 && l->rfind("At ", 0) != 0) it.error = *l;
        if (it.error.size() > 200) it.error = it.error.substr(0, 200) + "...";
        if (it.error.empty()) it.error = Tr(S::BuildFailedTitle);
    }
    next();
}

// a port's line under "Update and make all": what it did to that game, or what it is doing now
void BulkLine(int game) {
    const BulkRun& b = g_bulk;
    if (!b.running && !b.finished) return;
    for (size_t k = 0; k < b.items.size(); k++) {
        const BulkItem& it = b.items[k];
        if (it.game != game) continue;
        Game& gm = *g.games[size_t(game)];
        std::string text;
        ImVec4 color(0.45f, 0.85f, 0.5f, 1);
        if (b.running && b.pass == 1 && b.at == k && b.phase == 3) {
            GameBuild::Status bs = gm.build.Get();
            int total = int(gm.prof.install.steps.size()), cur = std::min(bs.step, total - 1);
            char head[256];
            if (bs.stage == GameBuild::Stage::Running)
                std::snprintf(head, sizeof(head), Tr(S::BuildRunning), bs.step + 1 > total ? total : bs.step + 1, total,
                              cur >= 0 ? Tx(gm.prof.install.steps[size_t(cur)].label) : "");
            else
                std::snprintf(head, sizeof(head), "%s", Tr(S::Unpacking));
            text = head;
            color = Accent(1.3f);
        } else if (!it.error.empty()) {
            char e[512];
            std::snprintf(e, sizeof(e), Tr(S::UpdateAllFailed), it.error.c_str());
            text = e;
            color = ImVec4(0.95f, 0.45f, 0.4f, 1);
        } else if (it.built) {
            text = Tr(S::BuildDoneTitle);
        } else if (it.current) {
            text = Tr(S::UpdateAllCurrent);
        } else if (it.needs_inputs) {
            text = Tr(S::UpdateAllNeedsInputs);
            color = ImGui::ColorConvertU32ToFloat4(StateColor("wip"));
        }
        if (text.empty()) return;
        ImGui::SameLine();
        ImGui::TextColored(color, "● %s", text.c_str());
        return;
    }
}

void SettingsUpdates() {
    ImGui::Text("%s: %s", Tr(S::Installed), g.hub.version.c_str());
    ImGui::Spacing();
    bool on_start = g.state.Bool("check_updates", true);
    RowLabel(Tr(S::CheckOnStart));
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Checkbox("##onstart", &on_start)) g.state.Set("check_updates", on_start);
    ImGui::Spacing();
    UpdateCheck u = g.updater.Get();
    ImGui::BeginDisabled(u.state == UpdateCheck::State::Checking);
    if (ImGui::Button(Tr(S::CheckNow))) g.updater.Start(g.hub.update_repo, g.hub.version);
    ImGui::EndDisabled();
    ImGui::Spacing();
    switch (u.state) {
        case UpdateCheck::State::Idle: break;
        case UpdateCheck::State::Checking: ImGui::TextUnformatted(Tr(S::Checking)); break;
        case UpdateCheck::State::UpToDate:
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "● %s (%s)", Tr(S::UpToDate), u.latest.c_str());
            break;
        case UpdateCheck::State::Newer:
            ImGui::TextColored(Accent(1.3f), "● %s: %s", Tr(S::NewVersion), u.latest.c_str());
            UpdateActions(u);
            break;
        case UpdateCheck::State::Downloading:
            ImGui::TextUnformatted(Tr(S::DownloadingUpdate));
            UpdateActions(u);
            break;
        case UpdateCheck::State::Ready:
            ImGui::TextUnformatted(Tr(S::InstallingUpdate));
            UpdateActions(u);
            break;
        case UpdateCheck::State::NoRepo: Dim(Tr(S::NoRepo)); break;
        case UpdateCheck::State::Failed:
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "● %s: %s", Tr(S::UpdateFailed), u.error.c_str());
            break;
    }

    // the installed ports: each one's version and whether a newer one is out, looked up again on demand
    SubHeading(Tr(S::PortUpdates));
    std::vector<int> installed = InstalledPorts();
    if (installed.empty()) {
        Dim(Tr(S::NoPortsInstalled));
    } else {
        bool checking = false;
        for (int i : installed) checking |= g.games[size_t(i)]->fetcher.Busy();
        ImGui::BeginDisabled(checking || g_bulk.running);
        if (ImGui::Button(Tr(S::CheckPortsNow)))
            for (int i : installed) {
                Game& gm = *g.games[size_t(i)];
                gm.port_checked = true;
                gm.fetcher.Check(gm.entry.Source(), InstalledPortVersion(gm.entry.dir));
            }
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::UpdateAll))) StartBulk();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", Tr(S::UpdateAllHelp));
        if (g_bulk.running) {
            ImGui::Spacing();
            const BulkItem& cur = g_bulk.items[std::min(g_bulk.at, g_bulk.items.size() - 1)];
            char head[256];
            std::snprintf(head, sizeof(head), Tr(g_bulk.pass == 0 ? S::UpdateAllPorts : S::UpdateAllGames), int(g_bulk.at) + 1,
                          int(g_bulk.items.size()), Tx(g.games[size_t(cur.game)]->prof.title));
            ImGui::TextColored(Accent(1.3f), "● %s", head);
            if (g_bulk.waiting) Dim(Tr(S::UpdateAllWaiting));
            else if (g_bulk.cancel) Dim(Tr(S::UpdateAllStopped));
            Dim(Tr(S::UpdateAllHelp));
            ImGui::PushID("bulk");
            ImGui::BeginDisabled(g_bulk.cancel);
            if (ImGui::Button(Tr(S::Cancel))) CancelBulk();
            ImGui::EndDisabled();
            ImGui::PopID();
        } else if (g_bulk.finished) {
            ImGui::Spacing();
            if (g_bulk.cancel)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("wip")), "● %s", Tr(S::UpdateAllStopped));
            else if (BulkFailed())
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "● %s", Tr(S::UpdateAllDoneErrors));
            else
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "● %s", Tr(S::UpdateAllDone));
        }
        ImGui::Spacing();
        for (int i : installed) {
            Game& gm = *g.games[size_t(i)];
            PortFetcher::Status ps = gm.fetcher.Get();
            std::string mine = InstalledPortVersion(gm.entry.dir), latest;
            // a branch's commit, shortened as GitHub shows it
            auto shown = [&](const std::string& v) { return gm.entry.branch.empty() || v.size() <= 7 ? v : v.substr(0, 7); };
            ImGui::PushID(i);
            ImGui::TextUnformatted(Tx(gm.prof.title));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mine.empty() ? "-" : shown(mine).c_str());
            ImGui::SameLine();
            if (ps.stage == PortFetcher::Stage::Checking) {
                ImGui::TextDisabled("%s", Tr(S::Checking));
            } else if (ps.stage == PortFetcher::Stage::Downloading || ps.stage == PortFetcher::Stage::Unpacking) {
                ImGui::TextColored(Accent(1.3f), "● %s", ps.stage == PortFetcher::Stage::Unpacking ? Tr(S::UnpackingPort) : Tr(S::DownloadingPort));
            } else if (ps.stage == PortFetcher::Stage::Failed) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "● %s: %s", Tr(S::PortFailed), ps.error.c_str());
            } else if (ps.stage == PortFetcher::Stage::Done) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "● %s", Tr(S::PortUpdated));
            } else if (PortHasUpdate(gm, latest)) {
                char b[128];
                std::snprintf(b, sizeof(b), Tr(S::PortNewer), shown(latest).c_str());
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("wip")), "● %s", b);
                ImGui::SameLine();
                ImGui::BeginDisabled(g.proc != nullptr);
                if (ImGui::SmallButton(Tr(S::UpdatePort))) gm.fetcher.Install(gm.entry.Source(), gm.entry.dir);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr(S::UpdOpen))) OpenGame(i);
            } else if (ps.stage == PortFetcher::Stage::Ready) {
                ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "● %s", Tr(S::PortUpToDate));
            }
            BulkLine(i);
            ImGui::PopID();
        }
    }

    // what changed, version by version: the launcher's, then each installed port's that has releases
    SubHeading(Tr(S::WhatsNewTitle));
    Dim(Tr(S::NotesIntro));
    ImGui::Spacing();
    NotesBlock(g.hub.update_repo, g.hub.version, true);
    for (auto& gp : g.games) {
        Game& gm = *gp;
        if (gm.entry.repo.empty() || !gm.entry.branch.empty() || !gm.ok || !Playable(gm, g.here) || !PortPresent(gm))
            continue;
        ImGui::Dummy(ImVec2(0, 6 * g.scale));
        ImGui::PushFont(g.bold, 19);
        ImGui::TextUnformatted(Tx(gm.prof.title));
        ImGui::PopFont();
        NotesBlock(gm.entry.repo, InstalledPortVersion(gm.entry.dir), false);
    }
}

void SettingsAbout() {
    ImGui::PushFont(g.bold, 24);
    ImGui::TextUnformatted(g.hub.name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("%s %s", Tr(S::Version), g.hub.version.c_str());
    ImGui::Spacing();
    Dim(Tr(S::HubAbout));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(S::Credits));
    ImGui::BulletText("SDL 3, Dear ImGui, toml++, stb_image, Inter (SIL OFL)");
    for (auto& gp : g.games)
        for (auto& [name, what] : gp->prof.credits) ImGui::BulletText("%s — %s", Tx(name), Tx(what));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(Tr(S::Legal));
    Dim(Tr(S::HubLegal));
    ImGui::Spacing();
    ImGui::Separator();
    // the owner's key: the administration tab appears with it
    ImGui::TextUnformatted(Tr(S::AdminKey));
    Dim(Tr(S::AdminHelp));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputText("##akey", g.admin_key, sizeof(g.admin_key), ImGuiInputTextFlags_Password);
    ImGui::SameLine();
    if (ImGui::Button(Tr(S::Save))) {
        // sealed to this Windows account: a copy of the file elsewhere opens nothing
        g.admin_token = g.admin_key;
        if (g.admin_token.empty()) g.state.Remove("admin_key");
        else g.state.Set("admin_key", Seal(g.admin_token));
        g.state.Remove("admin_token");
        g.admin_stale = true;
    }
}

int SettingsTabs() { return g.admin_token.empty() ? 5 : 6; }
void SettingsMods();
void AdminPage();

void SettingsPage() {
    Heading(Tr(S::LauncherSettings));
    static const S kTabs[] = {S::General, S::Updates, S::Perf, S::Mods, S::About, S::Admin};
    if (ImGui::BeginTabBar("hubset")) {
        for (int i = 0; i < SettingsTabs(); i++) {
            ImGuiTabItemFlags f = g.focus_tab && i == g.settings_tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(Tr(kTabs[i]), nullptr, f)) {
                if (!g.focus_tab) g.settings_tab = i;
                ImGui::Spacing();
                if (i == 0) SettingsGeneral();
                else if (i == 1) SettingsUpdates();
                else if (i == 2) SettingsPerf();
                else if (i == 3) SettingsMods();
                else if (i == 4) SettingsAbout();
                else AdminPage();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    g.focus_tab = false;
}

// ---------------------------------------------------------------------------------------------- the server

struct ConsoleName {
    const char* id;
    Text name;
};

const std::vector<ConsoleName>& Consoles() {
    static const std::vector<ConsoleName> kConsoles = [] {
        auto same = [](const char* s) {
            Text t;
            t.pt = t.en = t.es = s;
            return t;
        };
        Text mobile, other;
        mobile.pt = "Celular (Java)", mobile.en = "Mobile (Java)", mobile.es = "Móvil (Java)";
        other.pt = "Outro", other.en = "Other", other.es = "Otro";
        return std::vector<ConsoleName>{
            {"xbox", same("Xbox")}, {"x360", same("Xbox 360")}, {"ps1", same("PlayStation 1")}, {"ps2", same("PlayStation 2")},
            {"ps3", same("PlayStation 3")}, {"psp", same("PSP")}, {"vita", same("PS Vita")}, {"gc", same("GameCube")},
            {"wii", same("Wii")}, {"wiiu", same("Wii U")}, {"nds", same("Nintendo DS")}, {"3ds", same("Nintendo 3DS")},
            {"j2me", mobile}, {"outro", other}};
    }();
    return kConsoles;
}

std::string ConsoleLabel(const std::string& id) {
    for (const ConsoleName& c : Consoles())
        if (id == c.id) return c.name.Get(g.lang);
    return id;
}

const std::string& Server() { return g.hub.server_url; }

Headers ServerHeaders(bool admin) {
    Headers h{{"X-Otavix-Install", g.install_id}};
    if (admin) h.push_back({"Authorization", "Bearer " + g.admin_token});
    return h;
}

const char* kPeriods[] = {"semana", "mes", "tudo"};

int Field(const std::vector<std::string>& row, size_t i) { return i < row.size() ? std::atoi(row[i].c_str()) : 0; }
const std::string& Str(const std::vector<std::string>& row, size_t i) {
    static const std::string empty;
    return i < row.size() ? row[i] : empty;
}

// Week / Month / All time
bool PeriodTabs(const char* id, int& period) {
    bool changed = false;
    if (ImGui::BeginTabBar(id)) {
        const S names[] = {S::Week, S::Month, S::AllTime};
        for (int i = 0; i < 3; i++)
            if (ImGui::BeginTabItem(Tr(names[i]))) {
                if (period != i) {
                    period = i;
                    changed = true;
                }
                ImGui::EndTabItem();
            }
        ImGui::EndTabBar();
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------- reports

const S kCats[] = {S::CatCrash, S::CatGraphics, S::CatAudio, S::CatInput, S::CatPerf, S::CatOther};

std::string Stamp() {
    SDL_Time t = 0;
    SDL_GetCurrentTime(&t);
    SDL_DateTime d{};
    SDL_TimeToDateTime(t, &d, true);
    char s[32];
    std::snprintf(s, sizeof(s), "%04d-%02d-%02d_%02d%02d%02d", d.year, d.month, d.day, d.hour, d.minute, d.second);
    return s;
}

std::string ReportJson(Game& gm, const fs::path& log, std::string& log_text, std::string& config) {
    std::string log_name = log.empty() ? std::string() : U8(log.filename());
    if (g.rep_attach) {
        if (!log.empty()) log_text = FileTail(log, 256 * 1024);
        config = FileTail(gm.entry.dir / FromU8(gm.prof.settings), 64 * 1024);
        // the profile's other files go along in the same text, each under its own heading (the server keeps one log)
        for (const fs::path& f : ReportFiles(gm)) {
            if (log_text.size() > 700 * 1024) break;
            log_text += (log_text.empty() ? "" : "\n") + std::string("===== ") + U8(f.lexically_relative(gm.entry.dir)) + " =====\n";
            log_text += FileTail(f, 128 * 1024);
            if (log_name.empty()) log_name = "arquivos-do-jogo.txt";
        }
    }
    std::string last = gm.last_exit == INT_MIN ? std::string("-") : std::to_string(gm.last_exit);
    std::string j = "{";
    j += "\"launcher\":" + Json(g.hub.version);
    j += ",\"jogo\":" + Json(gm.entry.id);
    j += ",\"titulo\":" + Json(gm.prof.title.Get(Lang::Pt));
    j += ",\"plataforma\":" + Json(PlatformName(g.view));
    // the owner reads the kind in Portuguese
    j += ",\"categoria\":" + Json(T(Lang::Pt, kCats[std::clamp(g.rep_cat, 0, 5)]));
    j += ",\"descricao\":" + Json(g.rep_desc);
    j += ",\"contato\":" + Json(g.rep_contact);
    j += ",\"ultima_saida\":" + Json(last);
    // the computer, and the port's own version and the executable that runs (a game made here has a build per
    // processor kind)
    std::vector<std::pair<std::string, std::string>> sys = g.sysinfo;
    std::error_code ec_sys;
    if (std::string port = InstalledPortVersion(gm.entry.dir); !port.empty()) sys.push_back({"port", port});
    // the executable that runs (FindGame is the game's copy: "." for a port made in its own folder)
    if (std::string exe = GameExe(gm); !exe.empty() && fs::is_regular_file(gm.entry.dir / FromU8(exe), ec_sys))
        sys.push_back({"exe", exe});
    if (!gm.prof.game_file.empty())
        if (fs::path game = FindGame(gm); !game.empty()) sys.push_back({"jogo", U8(game.lexically_relative(gm.entry.dir))});
    // a port made on this PC: the port version the game was made with ("port" is the one downloaded; they differ when
    // the port was updated and the game not made again)
    if (gm.prof.install.kind == "build")
        if (std::string built = gm.store.GetOwn("built_version"); !built.empty()) sys.push_back({"gerado", built});
    j += ",\"sistema\":{";
    for (size_t i = 0; i < sys.size(); i++) j += (i ? "," : "") + Json(sys[i].first) + ":" + Json(sys[i].second);
    j += "}";
    if (!log_text.empty()) j += ",\"log_nome\":" + Json(log_name) + ",\"log\":" + Json(log_text);
    if (!config.empty()) j += ",\"config\":" + Json(config);
    return j + "}";
}

void CrashCard(Game& gm);
std::string IssueUrl(Game& gm, const std::string& kind, const std::string& what);
void OpenIssue(Game& gm, const std::string& kind, const std::string& what);

// ---------------------------------------------------------------------------------------------- performance pages

// the summary keys of a session's options JSON, "fps=60;resolution_scale=2"
std::string SummaryOf(const std::string& options) {
    std::string out;
    for (const char* key : kSummaryKeys) {
        std::string v = JsonString(options, key);
        double d = 0;
        if (v.empty() && JsonNumber(options, key, d)) {
            char b[32];
            std::snprintf(b, sizeof(b), "%g", d);
            v = b;
        }
        if (!v.empty()) out += (out.empty() ? "" : ";") + std::string(key) + "=" + v;
    }
    return out;
}

std::string DateText(int64_t when) {
    SDL_DateTime d{};
    SDL_TimeToDateTime(SDL_Time(when) * SDL_NS_PER_SECOND, &d, true);
    char b[32];
    std::snprintf(b, sizeof(b), "%02d/%02d/%04d %02d:%02d", d.day, d.month, d.year, d.hour, d.minute);
    return b;
}

void MachineLines() {
    const Machine& m = ThisMachine();
    ImGui::Text("%s: %s (%d/%d)%s", Tr(S::PerfCpu), m.cpu.c_str(), m.cores, m.threads, m.avx512 ? " · AVX-512" : "");
    ImGui::Text("%s: %s (%.0f GB) · %s", Tr(S::PerfGpu), m.gpu.c_str(), double(m.vram_mb) / 1024.0, m.driver.c_str());
    ImGui::Text("%s: %d GB · Windows %s", Tr(S::PerfRam), m.ram_gb, m.windows.c_str());
}

void SubHeading(const char* text) {
    ImGui::Dummy(ImVec2(0, 8 * g.scale));
    ImGui::PushFont(g.bold, 21);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

// a game's performance: this computer, its sessions here, and how it ran for those who share theirs
void PerfPage() {
    Game& gm = G();
    Heading(Tr(S::Perf));
    // the game's own word on what weighs and where to start, before the numbers
    if (!gm.prof.perf_note.Empty()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g.scale, 14 * g.scale));
        Card("perfnote", 0, [] {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(StateColor("wip")));
            ImGui::TextWrapped("%s", Tx(G().prof.perf_note));
            ImGui::PopStyleColor();
        });
        ImGui::PopStyleVar();
    }
    SubHeading(Tr(S::PerfThisPc));
    MachineLines();

    SubHeading(Tr(S::PerfMine));
    static std::vector<Session> mine;
    static std::string mine_game;
    static Uint64 mine_at = 0;
    if (mine_game != gm.entry.id || SDL_GetTicks() - mine_at > 3000) {
        mine = LoadHistory(PerfHistory(), gm.entry.id);
        mine_game = gm.entry.id;
        mine_at = SDL_GetTicks();
    }
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (mine.empty()) {
        Dim(Tr(S::PerfNone));
    } else if (ImGui::BeginTable("mine", 6, flags)) {
        for (S h : {S::PerfDate, S::Minutes, S::PerfAvg, S::PerfLow, S::PerfMin, S::PerfOptions})
            ImGui::TableSetupColumn(Tr(h));
        ImGui::TableHeadersRow();
        for (size_t k = 0; k < mine.size() && k < 20; k++) {
            const Session& s = mine[mine.size() - 1 - k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(DateText(s.when).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", s.minutes);
            ImGui::TableNextColumn();
            ImGui::TextColored(Accent(1.3f), "%.1f", s.fps_avg);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", s.fps_p1);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", s.fps_min);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(SummaryText(gm, SummaryOf(s.options)).c_str());
        }
        ImGui::EndTable();
    }

    SubHeading(Tr(S::PerfCommunity));
    Dim(Tr(S::PerfCommunityIntro));
    if (Server().empty()) {
        Dim(Tr(S::PerfServerOff));
        return;
    }
    HttpResult r;
    if (g.perf_sum_call.Get(&r) == Call::State::Done) {
        g.perf_rows = r.Ok() ? Lines(r.body) : std::vector<std::vector<std::string>>{};
        g.perf_msg = r.Ok() ? "" : std::string(Tr(S::SendFailed)) + ": " + r.error;
        g.perf_sum_call.Reset();
    }
    if (g.perf_sum_game != gm.entry.id && g.perf_sum_call.Get() == Call::State::Idle) {
        g.perf_sum_game = gm.entry.id;
        g.perf_rows.clear();
        g.perf_sum_call.Start("GET", Server() + "/v1/desempenho?jogo=" + PercentEncode(gm.entry.id), ServerHeaders(), "");
    }
    if (ImGui::SmallButton(Tr(S::Refresh))) g.perf_sum_game.clear();
    if (g.perf_rows.empty()) {
        Dim(g.perf_sum_call.Get() == Call::State::Busy ? Tr(S::Loading)
            : g.perf_msg.empty()                        ? Tr(S::PerfCommunityEmpty)
                                                        : g.perf_msg.c_str());
    } else if (ImGui::BeginTable("community", 7, flags)) {
        for (S h : {S::PerfGpu, S::PerfCpu, S::PerfOptions, S::PerfSessions, S::PerfAvg, S::PerfLow, S::PerfMin})
            ImGui::TableSetupColumn(Tr(h));
        ImGui::TableHeadersRow();
        for (const auto& row : g.perf_rows) {  // gpu, cpu, summary, sessions, computers, fps avg, 1%, min
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Str(row, 0).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Str(row, 1).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(SummaryText(gm, Str(row, 2)).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s (%s %s)", Str(row, 3).c_str(), Str(row, 4).c_str(), Tr(S::PerfMachines));
            ImGui::TableNextColumn();
            ImGui::TextColored(Accent(1.3f), "%s", Str(row, 5).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Str(row, 6).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Str(row, 7).c_str());
        }
        ImGui::EndTable();
    }
}

// the launcher's settings about performance: sharing, the on-screen meter, what is sent, and deleting it
void SettingsPerf() {
    HttpResult r;
    if (g.perf_del_call.Get(&r) == Call::State::Done) {
        g.perf_msg = r.Ok() ? Tr(S::PerfDeleted) : std::string(Tr(S::SendFailed)) + ": " + r.error;
        g.perf_del_call.Reset();
    }
    bool share = g.state.Str("perf_share", "") == "sim";
    RowLabel(Tr(S::PerfShare));
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Checkbox("##share", &share)) g.state.Set("perf_share", std::string(share ? "sim" : "nao"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.76f, 0.8f, 1));
    ImGui::TextWrapped("%s", Tr(S::PerfShareHelp));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    bool osd = g.state.Bool("osd", false);
    RowLabel(Tr(S::PerfOsd));
    if (ImGui::Checkbox("##osd", &osd)) g.state.Set("osd", osd);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.76f, 0.8f, 1));
    ImGui::TextWrapped("%s", Tr(S::PerfOsdHelp));
    ImGui::PopStyleColor();

    SubHeading(Tr(S::PerfThisPc));
    MachineLines();
    SubHeading(Tr(S::PerfLastReport));
    std::vector<Session> all = LoadHistory(PerfHistory());
    if (all.empty()) {
        Dim(Tr(S::PerfNoReport));
    } else {
        Session last = all.back();
        last.summary = SummaryOf(last.options);
        static std::string shown;
        shown = PrettyJson(ReportJson(ThisMachine(), last, g.install_id));
        ImGui::InputTextMultiline("##last", shown.data(), shown.size() + 1, ImVec2(-1, 220 * g.scale),
                                  ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::Spacing();
    if (ImGui::Button(Tr(S::PerfDeleteLocal))) {
        std::error_code ec;
        fs::remove(PerfHistory(), ec);
        g.perf_msg = Tr(S::PerfDeleted);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(Server().empty() || g.perf_del_call.Get() == Call::State::Busy);
    if (ImGui::Button(Tr(S::PerfDeleteServer)))
        g.perf_del_call.Start("DELETE", Server() + "/v1/desempenho", ServerHeaders(), "");
    ImGui::EndDisabled();
    if (!g.perf_msg.empty()) Dim(g.perf_msg.c_str());
}

constexpr size_t kMaxPhotos = 4;

// the pictures picked in the dialog (on SDL's thread): made JPEG on the frame
void PhotosPicked(void*, const char* const* files, int) {
    if (!files) return;
    std::lock_guard lock(g.dlg_m);
    for (int i = 0; files[i]; i++) g.rep_photo_picked.push_back(files[i]);
}

// the report's pictures, one request after the other once the report has its id (the page need not stay open)
void PhotoTick() {
    if (g.photo_report.empty()) return;
    HttpResult r;
    Call::State st = g.photo_call.Get(&r);
    if (st == Call::State::Busy) return;
    if (st == Call::State::Done) {
        if (!r.Ok()) g.photo_failed++, g.rep_photo_msg = r.error.empty() ? r.body : r.error;
        g.photo_call.Reset();
        g.photo_next++;
    }
    if (g.photo_next < g.photo_queue.size()) {
        const Photo& p = g.photo_queue[g.photo_next];
        g.photo_call.Start("POST", Server() + "/v1/report/foto?report=" + g.photo_report + "&nome=" + PercentEncode(p.name),
                           ServerHeaders(), p.bytes);
        return;
    }
    char b[200];
    int n = int(g.photo_queue.size());
    if (g.photo_failed) std::snprintf(b, sizeof(b), Tr(S::ReportPhotosFailed), n - int(g.photo_failed), n);
    else std::snprintf(b, sizeof(b), Tr(S::ReportPhotosSent), n);
    g.rep_msg = std::string(Tr(S::ReportSent)) + " " + b;
    g.photo_report.clear();
    g.photo_queue.clear();
}

// the report form's pictures: from a file or the clipboard (Win+Shift+S), listed with a button to drop each
void ReportPhotos() {
    std::vector<std::string> picked;
    {
        std::lock_guard lock(g.dlg_m);
        picked.swap(g.rep_photo_picked);
    }
    auto add = [](Photo&& p) {
        if (g.rep_photos.size() >= kMaxPhotos) {
            g.rep_photo_msg = Tr(S::ReportPhotoLimit);
            return;
        }
        g.rep_photos.push_back(std::move(p));
        g.rep_photo_msg.clear();
    };
    for (const std::string& f : picked) {
        Photo p;
        std::string err;
        if (PhotoFromFile(FromU8(f), p, err)) add(std::move(p));
        else g.rep_photo_msg = U8(FromU8(f).filename()) + ": " + err;
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr(S::ReportPhotos));
    Dim(Tr(S::ReportPhotosHelp));
    ImGui::BeginDisabled(g.rep_photos.size() >= kMaxPhotos || !g.photo_report.empty());
    if (ImGui::Button(Tr(S::ReportAddPhotos))) {
        static std::string name;
        static SDL_DialogFileFilter filter;
        name = Tr(S::ReportImages);
        filter = {name.c_str(), "png;jpg;jpeg;bmp;webp"};
        SDL_ShowOpenFileDialog(PhotosPicked, nullptr, g.win, &filter, 1, SDL_GetUserFolder(SDL_FOLDER_SCREENSHOTS), true);
    }
    ImGui::SameLine();
    // tests: LAUNCHER_REPORT_PASTE=1 presses Paste screenshot once
    static bool test_paste = SDL_getenv("LAUNCHER_REPORT_PASTE") != nullptr;
    bool paste = test_paste;
    test_paste = false;
    if (ImGui::Button(Tr(S::ReportPastePhoto)) || paste) {
        Photo p;
        std::string err;
        if (PhotoFromClipboard(p, err)) add(std::move(p));
        else g.rep_photo_msg = err.empty() ? Tr(S::ReportNoClipboard) : err;
    }
    ImGui::EndDisabled();
    for (size_t i = 0; i < g.rep_photos.size(); i++) {
        const Photo& p = g.rep_photos[i];
        ImGui::PushID(int(i) + 1000);
        ImGui::BulletText("%s · %dx%d · %s", p.name.c_str(), p.width, p.height, Bytes(p.bytes.size()).c_str());
        ImGui::SameLine();
        bool drop = ImGui::SmallButton(Tr(S::ModRemove));
        ImGui::PopID();
        if (drop) {
            g.rep_photos.erase(g.rep_photos.begin() + std::ptrdiff_t(i));
            break;
        }
    }
    if (!g.rep_photo_msg.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", g.rep_photo_msg.c_str());
}

void ReportPage() {
    Game& gm = G();
    if (g.sysinfo.empty()) g.sysinfo = SystemInfo();
    Heading(Tr(S::Report));
    Dim(Tr(S::ReportIntro));
    ImGui::Spacing();
    RowLabel(Tr(S::ReportCategory));
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::BeginCombo("##cat", Tr(kCats[std::clamp(g.rep_cat, 0, 5)]))) {
        for (int i = 0; i < 6; i++)
            if (ImGui::Selectable(Tr(kCats[i]), i == g.rep_cat)) g.rep_cat = i;
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(Tr(S::ReportDesc));
    ImGui::InputTextMultiline("##desc", g.rep_desc, sizeof(g.rep_desc), ImVec2(-1, 150 * g.scale));
    ImGui::Spacing();
    RowLabel(Tr(S::ReportContact));
    ImGui::InputText("##contact", g.rep_contact, sizeof(g.rep_contact));
    ImGui::Spacing();
    ImGui::Checkbox(Tr(S::ReportAttach), &g.rep_attach);
    ReportPhotos();

    // everything that goes along, before it goes
    fs::path log = GameLog(gm);
    if (ImGui::CollapsingHeader(Tr(S::ReportPreview))) {
        ImGui::BulletText("%s · %s · launcher %s", Tx(gm.prof.title), PlatformName(g.view), g.hub.version.c_str());
        if (gm.last_exit != INT_MIN) ImGui::BulletText("%s: %s %d", Tr(S::LastExit), Tr(S::ExitCode), gm.last_exit);
        for (auto& [k, v] : g.sysinfo) ImGui::BulletText("%s: %s", k.c_str(), v.c_str());
        if (g.rep_attach) {
            std::error_code ec;
            if (!log.empty()) ImGui::BulletText("%s (%s)", U8(log.filename()).c_str(), Bytes(fs::file_size(log, ec)).c_str());
            for (const fs::path& f : ReportFiles(gm))
                ImGui::BulletText("%s (%s)", U8(f.lexically_relative(gm.entry.dir)).c_str(), Bytes(fs::file_size(f, ec)).c_str());
            ImGui::BulletText("%s", gm.prof.settings.c_str());
        }
        for (const Photo& p : g.rep_photos) ImGui::BulletText("%s (%s)", p.name.c_str(), Bytes(p.bytes.size()).c_str());
    }
    ImGui::Spacing();

    HttpResult r;
    Call::State st = g.report_call.Get(&r);
    if (st == Call::State::Done) {
        if (r.Ok()) {
            g.rep_msg = Tr(S::ReportSent);
            g.rep_desc[0] = 0;
            // the pictures go next, to the id the server answered with ("ok\t<id>")
            std::vector<std::vector<std::string>> rows = Lines(r.body);
            std::string id = rows.empty() ? std::string() : Str(rows[0], 1);
            if (!g.rep_photos.empty() && !id.empty() && id.find_first_not_of("0123456789") == std::string::npos) {
                g.photo_report = id;
                g.photo_queue = std::move(g.rep_photos);
                g.rep_photos.clear();
                g.photo_next = g.photo_failed = 0;
                g.rep_msg = Tr(S::ReportSendingPhotos);
            }
        } else {
            g.rep_msg = std::string(Tr(S::SendFailed)) + ": " + r.error + "\n" + Tr(S::ReportSaved);
        }
        g.report_call.Reset();
    }
    bool busy = st == Call::State::Busy || !g.photo_report.empty();
    // tests: LAUNCHER_REPORT_SEND=1 presses Send once the test's picture is in
    static bool test_send = SDL_getenv("LAUNCHER_REPORT_SEND") != nullptr;
    bool press = test_send && !busy && (!g.rep_photos.empty() || !SDL_getenv("LAUNCHER_REPORT_PHOTO"));
    if (press) test_send = false;
    ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(busy ? Tr(S::Sending) : Tr(S::ReportSend), ImVec2(260 * g.scale, 0)) || press) {
        if (std::string(g.rep_desc).find_first_not_of(" \n\t") == std::string::npos) {
            g.rep_msg = Tr(S::ReportEmpty);
        } else {
            std::string log_text, config;
            std::string json = ReportJson(gm, log, log_text, config);
            // a copy stays on the computer either way
            fs::path dir = g.hub_dir / "reports";
            std::error_code ec;
            fs::create_directories(dir, ec);
            std::string stamp = Stamp();
            std::ofstream(dir / FromU8(stamp + "_" + gm.entry.id + ".json"), std::ios::binary) << json;
            for (const Photo& p : g.rep_photos)
                std::ofstream(dir / FromU8(stamp + "_" + gm.entry.id + "_" + p.name), std::ios::binary) << p.bytes;
            if (Server().empty()) g.rep_msg = Tr(S::ServerOff);
            else g.report_call.Start("POST", Server() + "/v1/report", ServerHeaders(), json);
        }
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(Tr(S::SendToGitHub))) {
        std::string what = std::string("**Tipo:** ") + T(Lang::Pt, kCats[std::clamp(g.rep_cat, 0, 5)]) + "\n\n" + g.rep_desc;
        OpenIssue(gm, T(Lang::Pt, kCats[std::clamp(g.rep_cat, 0, 5)]), what);
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(S::OpenReports))) OpenPath(g.hub_dir / "reports");
    if (!g.rep_msg.empty()) ImGui::TextWrapped("%s", g.rep_msg.c_str());

    // the owner's answers to this game's reports sent from here
    bool heading = false;
    for (const auto& row : g.answers) {
        if (Str(row, 1) != gm.entry.id || Str(row, 5).empty()) continue;
        if (!heading) {
            ImGui::Dummy(ImVec2(0, 10 * g.scale));
            ImGui::Separator();
            ImGui::PushFont(g.bold, 22);
            ImGui::TextUnformatted(Tr(S::AnswersHere));
            ImGui::PopFont();
            heading = true;
        }
        ImGui::Spacing();
        Dim((std::string(Tr(S::AnswerYourReport)) + " \"" + Str(row, 4) + "\"").c_str());
        ImGui::TextWrapped("%s", Str(row, 5).c_str());
    }
}

// ---------------------------------------------------------------------------------------------- port requests

void FetchRanking() {
    std::string u = Server() + "/v1/ranking?periodo=" + kPeriods[g.rank_period];
    if (g.rank_console >= 0) u += std::string("&console=") + Consoles()[size_t(g.rank_console)].id;
    g.rank_call.Start("GET", u, ServerHeaders(), "");
    g.rank_stale = false;
}

void Vote(const std::string& console, const std::string& name, const std::string& cover, const std::string& comment,
          Call* call = nullptr) {
    (call ? *call : g.vote_call).Start("POST", Server() + "/v1/pedidos", ServerHeaders(),
                      "{\"console\":" + Json(console) + ",\"nome\":" + Json(name) + ",\"capa\":" + Json(cover) +
                          ",\"comentario\":" + Json(comment) + "}");
}

// a box art in a box of this height, its own shape kept; a dark slot while it loads or when there is none
void CoverBox(const std::string& console, const std::string& cover, float h) {
    float aspect = 0.72f;
    SDL_Texture* t = g.covers.Get(console, cover, &aspect);
    float w = h * 0.72f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(30, 30, 36, 255), 4 * g.scale);
    if (t) {
        // fit inside, centred
        float iw = w, ih = w / aspect;
        if (ih > h) ih = h, iw = h * aspect;
        ImVec2 o(at.x + (w - iw) / 2, at.y + (h - ih) / 2);
        dl->AddImageRounded(Tex(t), o, ImVec2(o.x + iw, o.y + ih), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4 * g.scale);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void RequestsPage() {
    Heading(Tr(S::Requests));
    Dim(Tr(S::RequestsIntro));
    ImGui::Spacing();
    if (Server().empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("soon")), "●");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", Tr(S::RequestsOff));
        return;
    }
    // answers
    HttpResult r;
    if (g.rank_call.Get(&r) == Call::State::Done) {
        if (r.Ok()) {
            g.ranking = Lines(r.body);
            g.rank_error.clear();
        } else {
            g.rank_error = r.error;
        }
        g.rank_call.Reset();
    }
    if (g.vote_call.Get(&r) == Call::State::Done) {
        if (r.Ok()) {
            g.req_msg = r.body == "ja" ? Tr(S::AlreadyRequested) : Tr(S::Requested);
            g.req_name[0] = g.req_comment[0] = 0;
            g.req_pick = nullptr;
            g.req_free = false;
            g.rank_stale = true;
            g.mine_stale = true;
        } else {
            g.req_msg = std::string(Tr(S::SendFailed)) + ": " + r.error;
        }
        g.vote_call.Reset();
    }
    if (g.rank_stale && g.rank_call.Get() == Call::State::Idle) FetchRanking();
    // the player's own requests, and one taken back
    if (g.mine_call.Get(&r) == Call::State::Done) {
        if (r.Ok()) g.mine = Lines(r.body);
        g.mine_call.Reset();
    }
    if (g.withdraw_call.Get(&r) == Call::State::Done) {
        g.req_msg = r.Ok() ? Tr(S::RequestWithdrawn) : std::string(Tr(S::SendFailed)) + ": " + (r.error.empty() ? r.body : r.error);
        g.rank_stale = g.mine_stale = true;
        g.withdraw_call.Reset();
    }
    if (g.mine_stale && g.mine_call.Get() == Call::State::Idle) {
        g.mine_call.Start("GET", Server() + "/v1/pedidos/meus", ServerHeaders(), "");
        g.mine_stale = false;
    }

    // the form: the console, then a game of its catalog (or, for a console without one, any name)
    const auto& consoles = Consoles();
    const std::string console = consoles[size_t(g.req_console)].id;
    bool has_list = !g.catalogs.Of(console).empty();
    RowLabel(Tr(S::Console));
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::BeginCombo("##rc", consoles[size_t(g.req_console)].name.Get(g.lang).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < int(consoles.size()); i++)
            if (ImGui::Selectable(consoles[size_t(i)].name.Get(g.lang).c_str(), i == g.req_console) && i != g.req_console) {
                g.req_console = i;
                g.req_pick = nullptr;
                g.req_free = false;
            }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    float cover_h = 92 * g.scale;
    float x0 = ImGui::GetContentRegionAvail().x * 0.45f + ImGui::GetStyle().WindowPadding.x;
    RowLabel(Tr(S::GameName));
    if (g.req_pick) {
        // the chosen game: its cover and name, and a way to change it
        ImGui::SetCursorPosX(x0);
        CoverBox(console, g.req_pick->cover, cover_h);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(g.bold, 22);
        ImGui::TextWrapped("%s", g.req_pick->name.c_str());
        ImGui::PopFont();
        if (ImGui::SmallButton(Tr(S::Change))) g.req_pick = nullptr;
        ImGui::EndGroup();
    } else {
        ImGui::InputTextWithHint("##rn", has_list ? Tr(S::SearchHint) : "", g.req_name, sizeof(g.req_name));
        if (has_list && !g.req_free && std::strlen(g.req_name) >= 2) {
            auto hits = g.catalogs.Search(console, g.req_name, 6);
            for (size_t i = 0; i < hits.size(); i++) {
                ImGui::PushID(int(i));
                ImGui::SetCursorPosX(x0);
                ImVec2 at = ImGui::GetCursorPos();
                if (ImGui::Selectable("##hit", false, 0, ImVec2(ImGui::GetContentRegionAvail().x, cover_h * 0.62f))) g.req_pick = hits[i];
                ImGui::SetCursorPos(at);
                CoverBox(console, hits[i]->cover, cover_h * 0.62f);
                ImGui::SameLine();
                ImGui::SetCursorPosY(at.y + cover_h * 0.31f - ImGui::GetFontSize() * 0.5f);
                ImGui::TextUnformatted(hits[i]->name.c_str());
                ImGui::SetCursorPos(ImVec2(at.x, at.y + cover_h * 0.62f + ImGui::GetStyle().ItemSpacing.y));
                ImGui::PopID();
            }
            if (hits.empty()) {
                ImGui::SetCursorPosX(x0);
                ImGui::TextDisabled("%s", Tr(S::NotInList));
            }
            // a game the list does not have: as typed
            ImGui::SetCursorPosX(x0);
            if (ImGui::SmallButton(Tr(S::RequestTyped))) g.req_free = true;
        } else if (has_list && g.req_free) {
            ImGui::SetCursorPosX(x0);
            ImGui::TextDisabled("%s", Tr(S::TypedName));
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(S::BackToList))) g.req_free = false;
        }
    }
    ImGui::Spacing();
    RowLabel(Tr(S::Comment));
    ImGui::InputText("##rcm", g.req_comment, sizeof(g.req_comment));
    ImGui::Spacing();
    bool busy = g.vote_call.Get() == Call::State::Busy;
    bool typed = std::string(g.req_name).find_first_not_of(' ') != std::string::npos;
    bool ready = g.req_pick || ((!has_list || g.req_free) && typed);
    ImGui::PushStyleColor(ImGuiCol_Button, Accent(0.75f));
    ImGui::BeginDisabled(busy || !ready);
    if (ImGui::Button(busy ? Tr(S::Sending) : Tr(S::Request), ImVec2(200 * g.scale, 0))) {
        if (g.req_pick) Vote(console, g.req_pick->name, g.req_pick->cover, g.req_comment);
        else Vote(console, g.req_name, "", g.req_comment);
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (!g.req_msg.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(g.req_msg.c_str());
    }

    // what this launcher asked for, each with a button to take it back
    if (!g.mine.empty()) {
        SubHeading(Tr(S::MyRequests));
        for (size_t i = 0; i < g.mine.size(); i++) {
            const auto& row = g.mine[i];  // console, key, name, when, comment, cover
            ImGui::PushID(int(i) + 2000);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", Str(row, 2).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s · %s", ConsoleLabel(Str(row, 0)).c_str(), DateText(std::atoll(Str(row, 3).c_str())).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(g.withdraw_call.Get() == Call::State::Busy);
            if (ImGui::SmallButton(Tr(S::WithdrawRequest)))
                g.withdraw_call.Start("DELETE", Server() + "/v1/pedidos", ServerHeaders(),
                                      "{\"console\":" + Json(Str(row, 0)) + ",\"chave\":" + Json(Str(row, 1)) + "}");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

    // the rankings
    ImGui::Dummy(ImVec2(0, 10 * g.scale));
    ImGui::Separator();
    ImGui::PushFont(g.bold, 24);
    ImGui::TextUnformatted(Tr(S::MostRequested));
    ImGui::PopFont();
    if (PeriodTabs("periods", g.rank_period)) g.rank_stale = true;
    ImGui::SetNextItemWidth(320 * g.scale);
    std::string filter = g.rank_console < 0 ? Tr(S::AllConsoles) : consoles[size_t(g.rank_console)].name.Get(g.lang);
    if (ImGui::BeginCombo("##rf", filter.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(Tr(S::AllConsoles), g.rank_console < 0)) g.rank_console = -1, g.rank_stale = true;
        for (int i = 0; i < int(consoles.size()); i++)
            if (ImGui::Selectable(consoles[size_t(i)].name.Get(g.lang).c_str(), i == g.rank_console))
                g.rank_console = i, g.rank_stale = true;
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    if (!g.rank_error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", g.rank_error.c_str());
    if (g.ranking.empty()) {
        Dim(g.rank_call.Get() == Call::State::Busy ? Tr(S::Loading) : Tr(S::NoRequests));
        return;
    }
    int top = 1;
    for (const auto& row : g.ranking) top = std::max(top, Field(row, 3));
    float row_h = 64 * g.scale;
    if (ImGui::BeginTable("rank", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36 * g.scale);
        ImGui::TableSetupColumn("cover", ImGuiTableColumnFlags_WidthFixed, row_h * 0.72f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("console", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("votes", ImGuiTableColumnFlags_WidthStretch, 2);
        ImGui::TableSetupColumn("+1", ImGuiTableColumnFlags_WidthFixed, 60 * g.scale);
        for (size_t i = 0; i < g.ranking.size(); i++) {
            const auto& row = g.ranking[i];  // console, key, name, votes, cover
            ImGui::PushID(int(i));
            ImGui::TableNextRow(0, row_h);
            float mid = (row_h - ImGui::GetFrameHeight()) * 0.5f;
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mid);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%zu", i + 1);
            ImGui::TableNextColumn();
            CoverBox(Str(row, 0), Str(row, 4), row_h);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mid);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Str(row, 2).c_str());
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mid);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", ConsoleLabel(Str(row, 0)).c_str());
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mid);
            char label[48];
            std::snprintf(label, sizeof(label), "%d %s", Field(row, 3), Field(row, 3) == 1 ? Tr(S::OneVote) : Tr(S::Votes));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Accent(0.8f));
            ImGui::ProgressBar(float(Field(row, 3)) / float(top), ImVec2(-1, 0), label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + mid);
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("+1")) Vote(Str(row, 0), Str(row, 2), Str(row, 4), "");
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------------------------- the hub's search

// what the search bar holds, ready for matching; empty under two letters
std::string SearchTyped() {
    std::string k = SearchKey(g.search);
    return k.size() < 2 ? std::string() : k;
}

bool HasAllWords(const std::string& key, const std::string& q) {
    for (size_t a = 0; a < q.size();) {
        size_t e = q.find(' ', a);
        if (key.find(q.substr(a, e == std::string::npos ? std::string::npos : e - a)) == std::string::npos) return false;
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return true;
}

// the short names players type, word by word, as the catalogs write them ("gta 4": "grand theft auto 4")
std::string Expanded(const std::string& q) {
    static const std::map<std::string, std::string> kAliases = {
        {"ac", "assassin s creed"},   {"bf", "battlefield"},          {"cod", "call of duty"},
        {"dmc", "devil may cry"},     {"ff", "final fantasy"},        {"ffx", "final fantasy x"},
        {"gow", "god of war"},        {"gt", "gran turismo"},         {"gta", "grand theft auto"},
        {"kh", "kingdom hearts"},     {"kof", "king of fighters"},    {"mgs", "metal gear solid"},
        {"mk", "mortal kombat"},      {"nfs", "need for speed"},      {"pes", "pro evolution soccer"},
        {"pvz", "plants vs zombies"}, {"rdr", "red dead redemption"}, {"re", "resident evil"},
        {"sf", "street fighter"},     {"sotc", "shadow of the colossus"}, {"tlou", "the last of us"},
        {"tmnt", "teenage mutant ninja turtles"}, {"botw", "breath of the wild"}, {"oot", "ocarina of time"}};
    std::string out, w;
    auto flush = [&] {
        if (w.empty()) return;
        auto it = kAliases.find(w);
        out += (out.empty() ? "" : " ") + (it == kAliases.end() ? w : it->second);
        w.clear();
    };
    for (char c : q) {
        if (c == ' ') flush();
        else w += c;
    }
    flush();
    return out;
}

// the hub's games and the consoles' catalog games for what was typed, worked out again only when it changes; a short
// name is looked for as the full one first, then as typed
void RunSearch() {
    std::string q = SearchTyped();
    if (q == g.search_done) return;
    g.search_done = q;
    g.search_games.clear();
    g.search_catalog.clear();
    g.search_capped = 0;
    if (q.empty()) return;
    std::vector<std::string> queries{Expanded(q)};
    if (queries[0] != q) queries.push_back(q);
    // a catalog game is the hub's own when the name is the same, or ends with it ("Colin McRae - Dirt 2"), on any
    // console: the port plays on the PC whatever console it came from
    std::vector<std::string> own;
    for (size_t i = 0; i < g.games.size(); i++) {
        const Game& gm = *g.games[i];
        std::string key = SearchKey(Tx(gm.prof.title));
        own.push_back(key);
        for (const std::string& query : queries)
            if (HasAllWords(key + " " + SearchKey(gm.entry.id), query)) {
                g.search_games.push_back(int(i));
                break;
            }
    }
    auto owned = [&](const std::string& key) {
        for (const std::string& k : own)
            if (!k.empty() &&
                (key == k || (key.size() > k.size() && key.compare(key.size() - k.size() - 1, std::string::npos, " " + k) == 0)))
                return true;
        return false;
    };
    // the best first: the name itself, then names starting with it, then those containing it; the shorter first
    std::vector<std::pair<int, std::pair<std::string, const CatalogGame*>>> hits;
    std::set<std::string> seen;
    for (size_t qi = 0; qi < queries.size(); qi++) {
        const std::string& query = queries[qi];
        for (const ConsoleName& c : Consoles()) {
            for (const CatalogGame* cg : g.catalogs.Search(c.id, query, 60)) {
                if (owned(cg->key) || !seen.insert(std::string(c.id) + "/" + cg->key).second) continue;
                int score = cg->key == query                ? 0
                            : cg->key.rfind(query, 0) == 0  ? 1
                            : cg->key.find(query) != std::string::npos ? 2
                                                                        : 3;
                hits.push_back({int(qi) * 10000 + score * 1000 + int(std::min<size_t>(cg->key.size(), 999)), {c.id, cg}});
            }
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const size_t kShown = 48;
    if (hits.size() > kShown) {
        hits.resize(kShown);
        g.search_capped = kShown;
    }
    for (auto& h : hits) g.search_catalog.push_back(h.second);
}

// the bar, centred at the top of the window and clear of the sidebar (narrower for that, down to a point; past it,
// centred over the page); Enter takes the controller to the hits
void SearchBar(float left, float right, float y, float h) {
    float centre = ImGui::GetIO().DisplaySize.x * 0.5f;
    float half = std::min({280 * g.scale, centre - left, right - centre});
    float w = half * 2, x = centre - half;
    if (half < 170 * g.scale) {
        w = std::min(560 * g.scale, right - left);
        x = left + (right - left - w) * 0.5f;
    }
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushFont(g.body, 19);
    float icon = 30 * g.scale;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18 * g.scale + icon, std::max(0.0f, (h - ImGui::GetFontSize()) * 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.06f, 0.08f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.1f, 0.1f, 0.12f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.1f, 0.1f, 0.12f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.14f));
    if (g.focus_search) {
        ImGui::SetKeyboardFocusHere();
        g.focus_search = false;
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint("##search", Tr(S::SearchGames), g.search, sizeof(g.search),
                                 ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_EnterReturnsTrue))
        g.focus_page = true;
    // a magnifying glass in the padding
    ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(r0.x + 18 * g.scale + 7 * g.scale, (r0.y + r1.y) * 0.5f - 2 * g.scale);
    ImU32 col = ImGui::IsItemActive() ? IM_COL32(230, 230, 236, 255) : IM_COL32(150, 150, 160, 255);
    dl->AddCircle(c, 7 * g.scale, col, 0, 2 * g.scale);
    dl->AddLine(ImVec2(c.x + 5 * g.scale, c.y + 5 * g.scale), ImVec2(c.x + 10 * g.scale, c.y + 10 * g.scale), col, 2.5f * g.scale);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
}

// words on a rounded pill centred on a point
void Pill(ImDrawList* dl, ImVec2 centre, const char* text, ImU32 bg) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    float px = 12 * g.scale, py = 5 * g.scale;
    ImVec2 p0(centre.x - ts.x * 0.5f - px, centre.y - ts.y * 0.5f - py);
    ImVec2 p1(centre.x + ts.x * 0.5f + px, centre.y + ts.y * 0.5f + py);
    dl->AddRectFilled(p0, p1, bg, (p1.y - p0.y) * 0.5f);
    dl->AddText(ImVec2(p0.x + px, p0.y + py), IM_COL32(255, 255, 255, 255), text);
}

// a catalog game without a port: its box art greyed, the name under it; lit, it offers a port request, which a click
// sends at once (the same vote as the requests page, without a comment)
void CatalogTile(const std::string& console, const CatalogGame& cg, ImVec2 size, bool focus) {
    std::string id = console + "/" + cg.key;
    ImGui::PushID(id.c_str());
    ImVec2 at = ImGui::GetCursorScreenPos();
    if (focus) ImGui::SetKeyboardFocusHere();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    bool pressed = ImGui::Button("##tile", size);
    ImGui::PopStyleColor(3);
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    bool voted = g.search_voted.count(id) > 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = 10 * g.scale, art_h = size.x / 0.72f;
    ImVec2 a1(at.x + size.x, at.y + art_h);
    dl->AddRectFilled(at, a1, IM_COL32(32, 32, 38, 235), r);
    float aspect = 0.72f;
    // greyed and a little dark; in colour, still a little dark under the request, when lit
    if (SDL_Texture* t = g.covers.Get(console, cg.cover, &aspect, !hot)) {
        float iw = size.x, ih = size.x / aspect;
        if (ih > art_h) ih = art_h, iw = art_h * aspect;
        ImVec2 o(at.x + (size.x - iw) * 0.5f, at.y + (art_h - ih) * 0.5f), o1(o.x + iw, o.y + ih);
        dl->AddImageRounded(Tex(t), o, o1, ImVec2(0, 0), ImVec2(1, 1), hot ? IM_COL32(190, 190, 190, 255) : IM_COL32(150, 150, 150, 255), r);
    } else {
        // no box art (or not yet here): the name in the box
        ImGui::PushFont(g.bold, 17);
        dl->PushClipRect(at, a1, true);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 12 * g.scale, at.y + 34 * g.scale),
                    IM_COL32(150, 150, 158, 255), cg.name.c_str(), nullptr, size.x - 24 * g.scale);
        dl->PopClipRect();
        ImGui::PopFont();
    }
    ImGui::PushFont(g.body, 15);
    // the console on the art's corner
    std::string label = ConsoleLabel(console);
    ImVec2 ls = ImGui::CalcTextSize(label.c_str());
    ImVec2 l0(at.x + 8 * g.scale, at.y + 8 * g.scale);
    dl->AddRectFilled(l0, ImVec2(l0.x + ls.x + 14 * g.scale, l0.y + ls.y + 6 * g.scale), IM_COL32(0, 0, 0, 175), 6 * g.scale);
    dl->AddText(ImVec2(l0.x + 7 * g.scale, l0.y + 3 * g.scale), IM_COL32(215, 215, 222, 255), label.c_str());
    // the name under the art, two lines at most
    dl->PushClipRect(ImVec2(at.x, a1.y), ImVec2(at.x + size.x, at.y + size.y), true);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 2 * g.scale, a1.y + 6 * g.scale),
                IM_COL32(185, 185, 192, 255), cg.name.c_str(), nullptr, size.x - 4 * g.scale);
    dl->PopClipRect();
    ImVec2 mid(at.x + size.x * 0.5f, at.y + art_h * 0.5f);
    if (voted) Pill(dl, ImVec2(mid.x, a1.y - 22 * g.scale), Tr(S::RequestedMark), ImGui::GetColorU32(Accent(0.8f)));
    else if (hot) Pill(dl, mid, Tr(S::RequestPort), ImGui::GetColorU32(Accent(0.95f)));
    ImGui::PopFont();
    if (hot) {
        dl->AddRect(at, a1, ImGui::GetColorU32(Accent(1.3f)), r, 0, 2 * g.scale);
        if (!voted) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (pressed && !voted) {
        if (Server().empty()) {
            Toast(Tr(S::RequestsOff));
        } else if (g.search_vote.Get() != Call::State::Busy) {
            Vote(console, cg.name, cg.cover, "", &g.search_vote);
            g.search_vote_name = cg.name;
            g.search_vote_id = id;
            g.search_voted.insert(id);
        }
    }
    ImGui::PopID();
}

// the answer to a request sent from the search, as a message at the foot
void PollSearchVote() {
    HttpResult r;
    if (g.search_vote.Get(&r) != Call::State::Done) return;
    if (r.Ok()) {
        char b[320];
        std::snprintf(b, sizeof(b), r.body == "ja" ? Tr(S::AlreadyName) : Tr(S::RequestedName), g.search_vote_name.c_str());
        Toast(b);
        g.rank_stale = g.mine_stale = true;
    } else {
        Toast(std::string(Tr(S::SendFailed)) + ": " + r.error);
        g.search_voted.erase(g.search_vote_id);  // can be sent again
    }
    g.search_vote.Reset();
}

// the hits: the hub's games as their cards, then the catalog's games without a port
void SearchPage() {
    RunSearch();
    std::vector<int> shown;
    for (int i : g.search_games)
        if (PassesFilter(i)) shown.push_back(i);
    FilterButtons(shown.size());
    bool catalog = g.search_filter == 0 && !g.search_catalog.empty();
    if (shown.empty() && !catalog) {
        Dim(Tr(S::SearchNothing));
        return;
    }
    float avail = ImGui::GetContentRegionAvail().x;
    if (!shown.empty()) {
        Heading(Tr(S::SearchYours));
        // smaller cards than the library's: the catalog's hits come under them
        float gap = 20 * g.scale;
        int cols = std::max(1, int((avail + gap) / (300 * g.scale + gap)));
        float w = (avail - gap * float(cols - 1)) / float(cols), h = w * 9.0f / 16.0f;
        for (size_t k = 0; k < shown.size(); k++) {
            if (k % size_t(cols)) ImGui::SameLine(0, gap);
            int i = shown[k];
            if (GameCard(*g.games[size_t(i)], ImVec2(w, h), g.focus_page && k == 0)) OpenGame(i);
            if (k % size_t(cols) == size_t(cols - 1)) ImGui::Dummy(ImVec2(0, gap * 0.25f));
        }
        ImGui::Dummy(ImVec2(0, 8 * g.scale));
    }
    if (catalog) {
        if (shown.empty()) Heading(Tr(S::SearchNoPort));
        else SubHeading(Tr(S::SearchNoPort));
        ImGui::Spacing();
        float gap = 16 * g.scale;
        int cols = std::max(1, int((avail + gap) / (150 * g.scale + gap)));
        float w = (avail - gap * float(cols - 1)) / float(cols);
        ImVec2 size(w, w / 0.72f + 50 * g.scale);
        for (size_t k = 0; k < g.search_catalog.size(); k++) {
            if (k % size_t(cols)) ImGui::SameLine(0, gap);
            const auto& [console, cg] = g.search_catalog[k];
            CatalogTile(console, *cg, size, g.focus_page && shown.empty() && k == 0);
        }
        if (g.search_capped) {
            char b[160];
            std::snprintf(b, sizeof(b), Tr(S::SearchMore), int(g.search_capped));
            ImGui::Spacing();
            Dim(b);
        }
    }
}

// ---------------------------------------------------------------------------------------------- administration

void AdminPost(const char* what, const std::string& json) {
    g.admin_call.Start("POST", Server() + "/v1/admin/" + what, ServerHeaders(true), json);
}

void AdminPage() {
    if (Server().empty()) {
        Dim(Tr(S::RequestsOff));
        return;
    }
    HttpResult r;
    if (g.admin_rank_call.Get(&r) == Call::State::Done) {
        if (r.Ok()) g.admin_ranking = Lines(r.body), g.admin_msg.clear();
        else g.admin_msg = r.error;
        g.admin_rank_call.Reset();
    }
    if (g.admin_reports_call.Get(&r) == Call::State::Done) {
        if (r.Ok()) g.admin_reports = Lines(r.body);
        else g.admin_msg = r.error;
        g.admin_reports_call.Reset();
    }
    if (g.admin_call.Get(&r) == Call::State::Done) {
        if (!r.Ok()) g.admin_msg = r.error;
        g.admin_stale = true;
        g.admin_call.Reset();
    }
    // an attachment fetched: into the temporary folder and opened by its program
    if (g.admin_file_call.Get(&r) == Call::State::Done) {
        if (r.Ok()) {
            std::error_code ec;
            fs::path dir = fs::temp_directory_path(ec) / "otavix-anexos";
            fs::create_directories(dir, ec);
            fs::path file = dir / FromU8(g.admin_file_name);
            std::ofstream(file, std::ios::binary) << r.body;
            OpenPath(file);
        } else {
            g.admin_msg = r.error.empty() ? r.body : r.error;
        }
        g.admin_file_call.Reset();
    }
    if (g.admin_stale && g.admin_rank_call.Get() == Call::State::Idle) {
        g.admin_rank_call.Start("GET", Server() + "/v1/admin/ranking?periodo=" + kPeriods[g.admin_period], ServerHeaders(true), "");
        g.admin_reports_call.Start("GET", Server() + "/v1/admin/reports", ServerHeaders(true), "");
        g.admin_stale = false;
    }

    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button(Tr(S::Refresh))) g.admin_stale = true;
    if (!g.admin_msg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", g.admin_msg.c_str());
    }
    ImGui::PushFont(g.bold, 22);
    ImGui::TextUnformatted(Tr(S::Requests));
    ImGui::PopFont();
    if (PeriodTabs("aperiods", g.admin_period)) g.admin_stale = true;
    if (g.admin_ranking.empty()) Dim(g.admin_rank_call.Get() == Call::State::Busy ? Tr(S::Loading) : Tr(S::NoRequests));
    bool open_rename = false, open_merge = false, open_remove = false;
    if (!g.admin_ranking.empty() && ImGui::BeginTable("arank", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        // the buttons' column as wide as its buttons, in whatever language; the others share the rest (the console goes
        // under the name)
        const ImGuiStyle& style = ImGui::GetStyle();
        float actions = 0;
        for (S b : {S::Hide, S::Rename, S::Merge, S::RemoveRequest})
            actions += ImGui::CalcTextSize(Tr(b)).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
        actions = std::max(actions, actions - ImGui::CalcTextSize(Tr(S::Hide)).x + ImGui::CalcTextSize(Tr(S::Show)).x);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3);
        ImGui::TableSetupColumn("votes", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("comments", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, actions);
        for (size_t i = 0; i < g.admin_ranking.size(); i++) {
            const auto& row = g.admin_ranking[i];
            bool hidden = Field(row, 4) != 0;
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (hidden) ImGui::TextDisabled("%s (%s)", Str(row, 2).c_str(), Tr(S::Hidden));
            else ImGui::TextUnformatted(Str(row, 2).c_str());
            ImGui::TextDisabled("%s", ConsoleLabel(Str(row, 0)).c_str());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", Field(row, 3));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%d %s", Field(row, 5), Tr(S::Comments));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(hidden ? Tr(S::Show) : Tr(S::Hide)))
                AdminPost("ocultar", "{\"console\":" + Json(Str(row, 0)) + ",\"chave\":" + Json(Str(row, 1)) +
                                         ",\"oculto\":" + (hidden ? "false" : "true") + "}");
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(S::Rename))) {
                g.adm_console = Str(row, 0), g.adm_key = Str(row, 1);
                std::snprintf(g.adm_name, sizeof(g.adm_name), "%s", Str(row, 2).c_str());
                open_rename = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(S::Merge))) {
                g.adm_console = Str(row, 0), g.adm_key = Str(row, 1);
                open_merge = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(S::RemoveRequest))) {
                g.adm_remove_console = Str(row, 0), g.adm_remove_key = Str(row, 1), g.adm_remove_name = Str(row, 2);
                open_remove = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (open_rename) ImGui::OpenPopup("rename");
    if (open_merge) ImGui::OpenPopup("merge");
    if (open_remove) ImGui::OpenPopup("remove");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * g.scale, 14 * g.scale));
    if (ImGui::BeginPopup("rename")) {
        ImGui::SetNextItemWidth(420 * g.scale);
        ImGui::InputText("##an", g.adm_name, sizeof(g.adm_name));
        if (ImGui::Button(Tr(S::Save))) {
            AdminPost("renomear", "{\"console\":" + Json(g.adm_console) + ",\"chave\":" + Json(g.adm_key) +
                                      ",\"nome\":" + Json(g.adm_name) + "}");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::Cancel))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // the request this one joins: the same console's others
    if (ImGui::BeginPopup("merge")) {
        for (const auto& row : g.admin_ranking) {
            if (Str(row, 0) != g.adm_console || Str(row, 1) == g.adm_key) continue;
            if (ImGui::Selectable(Str(row, 2).c_str())) {
                AdminPost("juntar", "{\"console\":" + Json(g.adm_console) + ",\"de\":" + Json(g.adm_key) +
                                        ",\"para\":" + Json(Str(row, 1)) + "}");
                ImGui::CloseCurrentPopup();
            }
        }
        if (ImGui::Selectable(Tr(S::Cancel))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // removing is for good (hiding keeps the votes): asked once more
    if (ImGui::BeginPopup("remove")) {
        char q[256];
        std::snprintf(q, sizeof(q), Tr(S::RemoveRequestConfirm), g.adm_remove_name.c_str());
        ImGui::TextUnformatted(q);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1));
        if (ImGui::Button(Tr(S::RemoveRequest))) {
            AdminPost("remover", "{\"console\":" + Json(g.adm_remove_console) + ",\"chave\":" + Json(g.adm_remove_key) + "}");
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::Cancel))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0, 10 * g.scale));
    ImGui::Separator();
    ImGui::PushFont(g.bold, 22);
    ImGui::TextUnformatted(Tr(S::RecentReports));
    ImGui::PopFont();
    if (g.admin_reports.empty()) Dim(Tr(S::NoRequests));
    bool open_reply = false;
    for (size_t i = 0; i < g.admin_reports.size(); i++) {
        // when, title, platform, kind, contact, description, mailed, id, answer, attachments, computer
        const auto& row = g.admin_reports[i];
        SDL_DateTime d{};
        SDL_TimeToDateTime(SDL_Time(std::atoll(Str(row, 0).c_str())) * SDL_NS_PER_SECOND, &d, true);
        ImGui::PushID(int(i));
        ImGui::TextColored(Accent(1.3f), "%02d/%02d %02d:%02d", d.day, d.month, d.hour, d.minute);
        ImGui::SameLine();
        ImGui::Text("%s · %s · %s%s%s", Str(row, 1).c_str(), Str(row, 2).c_str(), Str(row, 3).c_str(),
                    Str(row, 4).empty() ? "" : " · ", Str(row, 4).c_str());
        if (Field(row, 6)) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", Tr(S::Mailed));
        }
        Dim(Str(row, 5).c_str());
        if (!Str(row, 10).empty()) ImGui::TextDisabled("%s", Str(row, 10).c_str());
        // the attachments, "id|name|bytes;...": each opens in its own program
        const std::string& files = Str(row, 9);
        for (size_t at = 0, n = 0; at < files.size(); n++) {
            size_t end = files.find(';', at);
            if (end == std::string::npos) end = files.size();
            std::string one = files.substr(at, end - at);
            at = end + 1;
            size_t a = one.find('|'), b = one.rfind('|');
            if (a == std::string::npos || b == a) continue;
            std::string fid = one.substr(0, a), name = one.substr(a + 1, b - a - 1);
            if (fid.empty() || fid.find_first_not_of("0123456789") != std::string::npos) continue;
            if (n) ImGui::SameLine();
            std::string label = name + " (" + Bytes(std::strtoull(one.c_str() + b + 1, nullptr, 10)) + ")##f" + fid;
            ImGui::BeginDisabled(g.admin_file_call.Get() == Call::State::Busy);
            if (ImGui::SmallButton(label.c_str())) {
                g.admin_file_name = fid + "_" + name;
                g.admin_file_call.Start("GET", Server() + "/v1/admin/anexo?id=" + fid, ServerHeaders(true), "");
            }
            ImGui::EndDisabled();
        }
        if (!Str(row, 8).empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Accent(1.3f));
            ImGui::TextWrapped("%s: %s", Tr(S::Answered), Str(row, 8).c_str());
            ImGui::PopStyleColor();
        }
        // the id comes from the server as digits; a server without answers sends none
        const std::string& id = Str(row, 7);
        if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos && ImGui::SmallButton(Tr(S::AnswerBtn))) {
            g.adm_reply_id = id;
            std::snprintf(g.adm_reply, sizeof(g.adm_reply), "%s", Str(row, 8).c_str());
            open_reply = true;
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (open_reply) ImGui::OpenPopup("reply");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * g.scale, 14 * g.scale));
    if (ImGui::BeginPopup("reply")) {
        Dim(Tr(S::AnswerLabel));
        ImGui::InputTextMultiline("##reply", g.adm_reply, sizeof(g.adm_reply), ImVec2(620 * g.scale, 150 * g.scale));
        if (ImGui::Button(Tr(S::AnswerSend))) {
            AdminPost("responder", "{\"id\":" + g.adm_reply_id + ",\"resposta\":" + Json(g.adm_reply) + "}");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(S::Cancel))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------------------------- mods

fs::path ModsDir(const Game& gm) { return gm.entry.dir / FromU8(gm.prof.mods_folder); }

void ZipPicked(void*, const char* const* files, int) {
    std::lock_guard lock(g.dlg_m);
    if (files && files[0]) {
        g.zip_path = files[0];
        g.zip_ready = true;
    }
}

// a .zip unpacked into its own folder in the mods folder, by the system's tar, away from the frame
void InstallZip(Game& gm, const fs::path& zip, bool keep_zip_aside) {
    if (gm.mods_busy) return;
    gm.mods_busy = true;
    gm.mods_msg = Tr(S::Installing);
    fs::path dir = ModsDir(gm);
    std::thread([&gm, dir, zip, keep_zip_aside] {
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path dest = dir / zip.stem();
        for (int i = 2; fs::exists(dest, ec); i++) dest = dir / FromU8(U8(zip.stem()) + " (" + std::to_string(i) + ")");
        fs::create_directories(dest, ec);
        std::string z = U8(zip), d = U8(dest);
        char tar[512];
        const char* sysroot = SDL_getenv("SystemRoot");
        std::snprintf(tar, sizeof(tar), "%s\\System32\\tar.exe", sysroot ? sysroot : "C:\\Windows");
        const char* argv[] = {tar, "-xf", z.c_str(), "-C", d.c_str(), nullptr};
        SDL_Process* p = SDL_CreateProcess(argv, false);
        int code = -1;
        if (p) {
            SDL_WaitProcess(p, true, &code);
            SDL_DestroyProcess(p);
        }
        bool ok = code == 0 && !fs::is_empty(dest, ec);
        if (ok) {
            FlattenMod(dest);
            // a .zip that sat in the mods folder goes aside once unpacked
            if (keep_zip_aside) {
                fs::create_directories(dir / "_zips", ec);
                fs::rename(zip, dir / "_zips" / zip.filename(), ec);
            }
        } else {
            fs::remove_all(dest, ec);
        }
        gm.mods_msg = ok ? Tr(S::ModInstalled) : std::string(Tr(S::ModInstallFailed)) + " (tar " + std::to_string(code) + ")";
        gm.mods_stale = true;
        gm.mods_busy = false;
    }).detach();
}

// a PSXRecomp option's widget; true when changed
bool PsxOptionWidget(PsxMods& m, const PsxPackage& p, const PsxOption& o) {
    std::string v = m.Get(p, o);
    bool changed = false;
    ImGui::PushID(o.id.c_str());
    if (o.type == "boolean") {
        bool on = v == "true" || v == "1";
        if (ImGui::Checkbox(o.label.c_str(), &on)) {
            m.Set(p, o, on ? "true" : "false");
            changed = true;
        }
    } else if (o.type == "choice" && !o.choices.empty()) {
        std::string shown = v;
        for (const PsxChoice& ch : o.choices)
            if (ch.value == v) shown = ch.label;
        ImGui::SetNextItemWidth(300 * g.scale);
        if (ImGui::BeginCombo(o.label.c_str(), shown.c_str())) {
            for (const PsxChoice& ch : o.choices)
                if (ImGui::Selectable(ch.label.c_str(), ch.value == v) && ch.value != v) {
                    m.Set(p, o, ch.value);
                    changed = true;
                }
            ImGui::EndCombo();
        }
    } else if (o.type == "integer") {
        int x = std::atoi(v.c_str());
        ImGui::SetNextItemWidth(300 * g.scale);
        if (ImGui::SliderInt(o.label.c_str(), &x, int(o.min), int(o.max))) {
            int64_t step = std::max<int64_t>(1, o.step);
            x = int(o.min + (int64_t(x) - o.min + step / 2) / step * step);
            m.Set(p, o, std::to_string(x));
            changed = true;
        }
    } else {
        ImGui::TextUnformatted((o.label + ": " + v).c_str());
    }
    if (!o.description.empty()) ImGui::SetItemTooltip("%s", o.description.c_str());
    ImGui::PopID();
    return changed;
}

// the mods of a port made with PSXRecomp: its packages' features, the game's own first, switched on and off with
// their options; the game's mods/state.toml written at each change
void PsxModsPage(Game& gm) {
    Heading(Tr(S::Mods));
    Dim(gm.prof.mods_help.Empty() ? Tr(S::ModsPsxIntro) : Tx(gm.prof.mods_help));
    ImGui::Spacing();
    PsxMods& m = gm.psx_mods;
    if (!m.Loaded() || gm.mods_stale) {
        m.Load(gm.entry.dir / FromU8(gm.prof.mods_folder), gm.prof.mods_game_id);
        gm.mods_stale = false;
    }
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button(Tr(S::OpenModsFolder))) OpenPath(m.Folder());
    ImGui::SameLine();
    if (ImGui::Button(Tr(S::Refresh))) gm.mods_stale = true;
    if (!m.Error().empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", m.Error().c_str());
    if (m.Packages().empty()) {
        char b[600];
        std::snprintf(b, sizeof(b), Tr(S::ModsPsxNone), U8(m.Folder()).c_str());
        Dim(b);
        return;
    }
    bool changed = false;
    int section = -1;
    for (const PsxPackage& p : m.Packages()) {
        int s = p.OfTheGame() ? 0 : 1;
        if (s != section) {
            section = s;
            SubHeading(s == 0 ? Tr(S::ModsPsxGame) : Tr(S::ModsPsxFramework));
            ImGui::Spacing();
        }
        ImGui::PushID(p.id.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * g.scale, 12 * g.scale));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.11f, 0.85f));
        ImGui::BeginChild("pkg", ImVec2(-1, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        ImGui::PushFont(g.bold, 20);
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s · %s", p.author.c_str(), p.version.c_str());
        if (!p.description.empty()) Dim(p.description.c_str());
        for (const PsxFeature& f : p.features) {
            ImGui::PushID(f.id.c_str());
            bool on = m.Enabled(p, f);
            ImGui::PushFont(g.bold, 18);
            if (ImGui::Checkbox(f.name.c_str(), &on)) {
                m.SetEnabled(p, f, on);
                changed = true;
            }
            ImGui::PopFont();
            if (!f.description.empty() && f.description != p.description) Dim(f.description.c_str());
            if (on) {
                ImGui::Indent();
                for (const PsxOption& o : p.options)
                    if (o.feature == f.id) changed |= PsxOptionWidget(m, p, o);
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (changed && !m.Save()) Toast(Tr(S::SaveFailed));
}

void ModsPage() {
    Game& gm = G();
    if (gm.prof.mods_kind == "psxrecomp") {
        PsxModsPage(gm);
        return;
    }
    Heading(Tr(S::Mods));
    Dim(gm.prof.mods_help.Empty() ? Tr(S::ModsHelp) : Tx(gm.prof.mods_help));
    ImGui::Spacing();
    if (!g.state.Bool("mods_on", true)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("wip")), "●");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", Tr(S::ModsOff));
    }
    fs::path dir = ModsDir(gm);
    {
        std::lock_guard lock(g.dlg_m);
        if (g.zip_ready) {
            g.zip_ready = false;
            InstallZip(gm, FromU8(g.zip_path), false);
        }
    }
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button(Tr(S::OpenModsFolder))) OpenPath(dir);
    ImGui::SameLine();
    ImGui::BeginDisabled(gm.mods_busy);
    if (ImGui::Button(Tr(S::InstallZip))) {
        static const SDL_DialogFileFilter kZip[] = {{"Mod (.zip)", "zip"}};
        std::string start = U8(dir);
        SDL_ShowOpenFileDialog(ZipPicked, nullptr, g.win, kZip, 1, start.c_str(), false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(S::Refresh))) gm.mods_stale = true;
    if (!gm.mods_msg.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(gm.mods_msg.c_str());
    }
    if (gm.mods_stale && !gm.mods_busy) {
        gm.mods = ScanMods(dir);
        gm.mods_stale = false;
    }
    ImGui::Spacing();
    if (gm.mods.empty()) {
        Dim(Tr(S::NoMods));
        return;
    }
    bool changed = false;
    float pad = 14 * g.scale;
    for (size_t i = 0; i < gm.mods.size(); i++) {
        Mod& m = gm.mods[i];
        ImGui::PushID(int(i));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad * 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, m.active ? Accent(0.35f, 0.55f) : ImVec4(0.11f, 0.11f, 0.13f, 0.92f));
        ImGui::BeginChild("mod", ImVec2(-1, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        if (m.zip) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Tx(m.name));
            ImGui::SameLine();
            ImGui::TextDisabled("(.zip, %s)", Bytes(m.bytes).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(gm.mods_busy);
            if (ImGui::Button(Tr(S::ModInstall))) InstallZip(gm, dir / FromU8(m.folder), true);
            ImGui::EndDisabled();
        } else {
            if (ImGui::Checkbox("##on", &m.active)) changed = true;
            ImGui::SameLine();
            ImGui::PushFont(g.bold, 20);
            ImGui::TextUnformatted(Tx(m.name));
            ImGui::PopFont();
            std::string meta;
            if (!m.version.empty()) meta += m.version;
            if (!m.author.empty()) meta += (meta.empty() ? "" : " · ") + m.author;
            char files[64];
            std::snprintf(files, sizeof(files), Tr(S::ModFiles), int(m.files.size()));
            meta += (meta.empty() ? "" : " · ") + std::string(files) + " · " + Bytes(m.bytes);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", meta.c_str());
            // order among the active ones, and removal
            float right = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(ImGui::GetCursorPosX() + std::max(0.0f, right - 230 * g.scale));
            ImGui::BeginDisabled(!m.active || i == 0 || !gm.mods[i - 1].active);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                std::swap(gm.mods[i], gm.mods[i - 1]);
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!m.active || i + 1 >= gm.mods.size() || !gm.mods[i + 1].active);
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                std::swap(gm.mods[i], gm.mods[i + 1]);
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (gm.confirm_remove == int(i)) {
                if (ImGui::SmallButton(Tr(S::Yes))) {
                    std::string err;
                    if (!RemoveMod(dir, gm.mods[i], err)) gm.mods_msg = err;
                    gm.confirm_remove = -1;
                    gm.mods_stale = true;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr(S::No))) gm.confirm_remove = -1;
            } else if (ImGui::SmallButton(Tr(S::ModRemove))) {
                gm.confirm_remove = int(i);
            }
            if (gm.confirm_remove == int(i)) Dim(Tr(S::ModRemoveConfirm));
            if (!m.description.Empty()) Dim(Tx(m.description));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (changed) {
        // the active ones keep their order above the others
        std::stable_partition(gm.mods.begin(), gm.mods.end(), [](const Mod& m) { return m.active; });
        if (!SaveActiveMods(dir, gm.mods)) gm.mods_msg = Tr(S::SaveFailed);
    }
    auto conflicts = ModConflicts(gm.mods);
    if (!conflicts.empty()) {
        char head[160];
        std::snprintf(head, sizeof(head), Tr(S::ModConflicts), int(conflicts.size()));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(StateColor("wip")), "● %s", head);
        if (ImGui::TreeNode(Tr(S::ModConflictFiles))) {
            for (auto& [file, holders] : conflicts) {
                std::string who;
                for (const std::string& h : holders) who += (who.empty() ? "" : " > ") + h;
                ImGui::BulletText("%s  (%s)", file.c_str(), who.c_str());
            }
            ImGui::TreePop();
        }
    }
}

// the launcher's settings: mods everywhere on or off, and each port's mods at a glance
void SettingsMods() {
    bool on = g.state.Bool("mods_on", true);
    RowLabel(Tr(S::UseMods));
    if (g.focus_page) ImGui::SetKeyboardFocusHere();
    if (ImGui::Checkbox("##modson", &on)) g.state.Set("mods_on", on);
    ImGui::Spacing();
    Dim(Tr(S::ModsGeneralHelp));
    ImGui::Spacing();
    ImGui::Separator();
    bool any = false;
    for (size_t i = 0; i < g.games.size(); i++) {
        Game& gm = *g.games[i];
        if (!gm.ok || gm.prof.mods_folder.empty() || !Playable(gm, g.here)) continue;
        any = true;
        if (gm.mods_stale && !gm.mods_busy) {
            gm.mods = ScanMods(ModsDir(gm));
            gm.mods_stale = false;
        }
        int active = 0;
        for (const Mod& m : gm.mods) active += m.active;
        ImGui::PushID(int(i));
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(g.bold, 20);
        ImGui::TextUnformatted(Tx(gm.prof.title));
        ImGui::PopFont();
        ImGui::SameLine();
        char count[96];
        std::snprintf(count, sizeof(count), Tr(S::ModsCount), int(gm.mods.size()), active);
        ImGui::TextDisabled("%s", count);
        ImGui::SameLine();
        if (ImGui::SmallButton(Tr(S::Manage))) {
            OpenGame(int(i));
            std::vector<Entry> entries = Entries();
            for (size_t k = 0; k < entries.size(); k++)
                if (entries[k].kind == Kind::Mods) G().page = int(k);
        }
        ImGui::PopID();
    }
    if (!any) Dim(Tr(S::NoModGames));
}

// ---------------------------------------------------------------------------------------------- frame

bool Idle() { return !g.cap_slot && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) && !ImGui::IsAnyItemActive(); }

// shoulders, or Page Up / Down, turn the pages; triggers, or Q / E, the tabs; B or Escape goes back
// the filter's buttons are on screen: a library page, or the search's results
bool FilterShown() {
    if (!SearchTyped().empty()) return true;
    HubEntry e = HubEntries()[size_t(std::clamp(g.entry, 0, int(HubEntries().size()) - 1))];
    return !e.settings && !e.requests && !e.tutorial;
}

void Keys() {
    if (!Idle()) return;
    int dir = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) dir = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) dir = 1;
    int tab = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)) tab = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) || ImGui::IsKeyPressed(ImGuiKey_E, false)) tab = 1;
    if (g.screen == Screen::Game) {
        int n = int(Entries().size());
        if (dir) {
            G().page = (G().page + dir + n) % n;
            g.focus_page = true;
        }
        Platform other = g.view == Platform::Windows ? Platform::Linux : Platform::Windows;
        if (tab && Current().kind != Kind::Advanced && !G().StateOn(other, g.here).empty()) {
            g.view = other;
            G().page = 0;
            g.focus_page = true;
        }
        if (tab && Current().kind == Kind::Advanced) {
            int t = int(AdvancedPages().size());
            if (t) {
                G().tab = (G().tab + tab + t) % t;
                g.focus_tab = true;
                g.focus_page = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            BackToHub();
    } else {
        int n = int(HubEntries().size());
        if (dir) {
            g.entry = (g.entry + dir + n) % n;
            g.focus_page = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) ||
            (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)))
            g.focus_search = true;
        // the library's filter, round its choices
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) && FilterShown()) NextFilter();
        if (tab && HubEntries()[size_t(g.entry)].settings) {
            g.settings_tab = (g.settings_tab + tab + SettingsTabs()) % SettingsTabs();
            g.focus_tab = true;
            g.focus_page = true;
        }
    }
}

void Sidebar(float side, float height, float pad) {
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g.scale, 18 * g.scale));
    ImGui::BeginChild("side", ImVec2(side, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
    if (g.screen == Screen::Game) {
        ImGui::PushFont(g.body, 17);
        if (ImGui::Selectable((std::string("‹  ") + Tr(S::BackToLibrary)).c_str(), false, 0, ImVec2(0, 34 * g.scale))) BackToHub();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6 * g.scale));
        ImGui::PushFont(g.bold, 22);
        ImGui::TextWrapped("%s", Tx(G().prof.title));
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6 * g.scale));
        // the version: Windows or Linux/SteamOS, each with its state's dot
        ImGui::PushFont(g.bold, 14);
        ImGui::TextDisabled("%s", Tr(S::Version2));
        ImGui::PopFont();
        float half = (ImGui::GetContentRegionAvail().x - 6 * g.scale) / 2;
        for (Platform p : {Platform::Windows, Platform::Linux}) {
            std::string st = G().StateOn(p, g.here);
            bool sel = p == g.view;
            if (p == Platform::Linux) ImGui::SameLine(0, 6 * g.scale);
            ImGui::PushID(int(p));
            ImGui::PushStyleColor(ImGuiCol_Button, sel ? Accent(0.6f) : ImVec4(0.16f, 0.16f, 0.19f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? Accent(0.75f) : ImVec4(0.21f, 0.21f, 0.25f, 1));
            ImGui::PushFont(sel ? g.bold : g.body, 15);
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::BeginDisabled(st.empty());
            if (ImGui::Button(PlatformName(p), ImVec2(half, 38 * g.scale)) && !sel) {
                g.view = p;
                G().page = 0;
                g.focus_page = true;
            }
            ImGui::EndDisabled();
            ImGui::PopFont();
            ImGui::PopStyleColor(2);
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + half - 9 * g.scale, at.y + 9 * g.scale), 4 * g.scale,
                                                        StateColor(st));
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 10 * g.scale));
        std::vector<Entry> entries = Entries();
        for (int i = 0; i < int(entries.size()); i++) {
            ImGui::PushID(i);
            ImGui::PushFont(i == G().page ? g.bold : g.body, 20);
            if (ImGui::Selectable(PageName(entries[size_t(i)]), i == G().page, 0, ImVec2(0, 40 * g.scale)) && i != G().page) {
                G().page = i;
                g.focus_page = false;
            }
            ImGui::PopFont();
            ImGui::PopID();
        }
    } else {
        // the brand: the symbol, then the name
        if (SDL_Texture* icon = BrandIcon()) {
            float side = 58 * g.scale;
            ImGui::Image(Tex(icon), ImVec2(side, side));
            ImGui::SameLine(0, 12 * g.scale);
        }
        ImGui::BeginGroup();
        ImGui::PushFont(g.title, 34);
        ImGui::TextColored(Accent(1.25f), "Otavix");
        ImGui::PopFont();
        ImGui::PushFont(g.bold, 18);
        ImGui::TextDisabled("LAUNCHER");
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, 14 * g.scale));
        std::vector<HubEntry> entries = HubEntries();
        for (int i = 0; i < int(entries.size()); i++) {
            const HubEntry& e = entries[size_t(i)];
            if (e.tutorial) {
                ImGui::Dummy(ImVec2(0, 6 * g.scale));
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 6 * g.scale));
            }
            std::string label;
            char count[16] = "";
            if (e.settings) {
                label = Tr(S::LauncherSettings);
            } else if (e.requests) {
                label = Tr(S::Requests);
            } else if (e.tutorial) {
                label = Tr(S::Tutorial);
            } else {
                std::snprintf(count, sizeof(count), "%d", GamesIn(e.library));
                label = std::string(e.library < 0 ? Tr(S::AllGames) : Tx(g.hub.libraries[size_t(e.library)].name));
            }
            ImGui::PushID(i);
            ImGui::PushFont(i == g.entry ? g.bold : g.body, 19);
            // one line each, a library's game count on the right
            if (ImGui::Selectable(label.c_str(), i == g.entry, 0, ImVec2(0, 40 * g.scale)) && i != g.entry) {
                g.entry = i;
                g.focus_page = false;
            }
            if (count[0]) {
                ImVec2 mx = ImGui::GetItemRectMax(), mn = ImGui::GetItemRectMin();
                ImVec2 ts = ImGui::CalcTextSize(count);
                ImGui::GetWindowDrawList()->AddText(ImVec2(mx.x - ts.x - 8 * g.scale, (mn.y + mx.y - ts.y) * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), count);
            }
            ImGui::PopFont();
            ImGui::PopID();
        }
        // exit at the foot
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), height - 60 * g.scale));
        ImGui::PushFont(g.body, 18);
        if (ImGui::Selectable(Tr(S::Exit), false, 0, ImVec2(0, 36 * g.scale))) g.quit = true;
        ImGui::PopFont();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Frame() {
    ImGuiIO& io = ImGui::GetIO();
    Keys();
    g.help.clear();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* bg = ImGui::GetWindowDrawList();
    ImVec2 size = io.DisplaySize;
    if (g.screen == Screen::Game) DrawArt(bg, size, PageArt(G()), G().art_spec.seconds);
    else DrawArt(bg, size, {}, 8);

    float side = std::clamp(size.x * 0.22f, 220 * g.scale, 340 * g.scale);
    float foot = 54 * g.scale;
    float pad = 26 * g.scale;
    Sidebar(side, size.y - foot - pad * 2, pad);

    // the search bar on the hub; going to another of the hub's pages clears it
    static int search_entry = -1;
    if (g.entry != search_entry) {
        if (search_entry >= 0) g.search[0] = 0;
        search_entry = g.entry;
    }
    PollSearchVote();
    float bar = 0;
    if (g.screen == Screen::Hub && g.hub_ok) {
        float bar_h = 44 * g.scale;
        SearchBar(pad * 2 + side, size.x - pad, pad, bar_h);
        bar = bar_h + 8 * g.scale;
    }

    // content
    ImGui::SetCursorPos(ImVec2(pad * 2 + side, pad + bar));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g.scale, 18 * g.scale));
    float help_h = 76 * g.scale;
    bool game = g.screen == Screen::Game;
    Entry cur = game ? Current() : Entry{Kind::Home};
    bool has_help_box = game && (cur.kind == Kind::Page || cur.kind == Kind::Advanced);
    ImGui::BeginChild("content", ImVec2(size.x - side - pad * 3, size.y - foot - pad * 2 - bar - (has_help_box ? help_h : 0)),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(g.body, 19);
    if (!g.hub_ok) {
        Heading(g.hub.name.c_str());
        ImGui::TextWrapped("%s", g.hub_error.c_str());
    } else if (!game) {
        HubEntry e = HubEntries()[size_t(std::clamp(g.entry, 0, int(HubEntries().size()) - 1))];
        if (!SearchTyped().empty()) SearchPage();
        else if (e.settings) SettingsPage();
        else if (e.requests) RequestsPage();
        else if (e.tutorial) TutorialPage();
        else LibraryPage(e.library);
    } else if (!G().ok) {
        Heading(Tr(S::NoProfile));
        ImGui::TextWrapped("%s", G().error.c_str());
    } else if (cur.kind == Kind::Home) {
        HomePage();
    } else if (cur.kind == Kind::Saves) {
        SavesPageUi();
    } else if (cur.kind == Kind::Mods) {
        ModsPage();
    } else if (cur.kind == Kind::Perf) {
        PerfPage();
    } else if (cur.kind == Kind::News) {
        NewsPage(G());
    } else if (cur.kind == Kind::Report) {
        ReportPage();
    } else if (cur.kind == Kind::About) {
        AboutPageUi();
    } else if (cur.kind == Kind::Advanced) {
        AdvancedPage();
    } else {
        OptionsPage(G().prof.pages[size_t(cur.page)]);
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // the focused option's help under the page
    if (has_help_box) {
        ImGui::SetCursorPos(ImVec2(pad * 2 + side, size.y - foot - pad - help_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g.scale, 12 * g.scale));
        ImGui::BeginChild("help", ImVec2(size.x - side - pad * 3, help_h - 8 * g.scale), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.76f, 0.8f, 1));
        ImGui::TextWrapped("%s", g.help.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // footer: the controller's hints, unsaved state, messages
    ImGui::SetCursorPos(ImVec2(pad, size.y - foot));
    Hint("A", IM_COL32(74, 160, 70, 255), game ? Tr(S::HintSelect) : Tr(S::HintOpen));
    if (game) Hint("B", IM_COL32(200, 60, 50, 255), Tr(S::HintBack));
    Hint("LB", IM_COL32(90, 90, 100, 255), "");
    Hint("RB", IM_COL32(90, 90, 100, 255), game ? Tr(S::HintPages) : Tr(S::Library));
    if (!game) Hint("Y", IM_COL32(205, 160, 40, 255), Tr(S::HintSearch));
    if (!game && FilterShown()) Hint("X", IM_COL32(60, 110, 200, 255), Tr(S::HintFilter));
    bool settings = !game && HubEntries()[size_t(std::clamp(g.entry, 0, int(HubEntries().size()) - 1))].settings;
    if ((game && cur.kind == Kind::Advanced) || settings) {
        Hint("LT", IM_COL32(90, 90, 100, 255), "");
        Hint("RT", IM_COL32(90, 90, 100, 255), Tr(S::HintTabs));
    } else if (game) {
        Hint("LT", IM_COL32(90, 90, 100, 255), "");
        Hint("RT", IM_COL32(90, 90, 100, 255), Tr(S::HintPlatform));
    }
    if (game && G().store.Dirty()) {
        ImGui::TextColored(Accent(1.3f), "%s", Tr(S::Unsaved));
        ImGui::SameLine();
    }
    if (!g.toast.empty() && SDL_GetTicks() < g.toast_until) ImGui::TextUnformatted(g.toast.c_str());
    ImGui::End();
    Notices();

    // a game's changes are kept as they are made, a moment after the last one
    static Uint64 dirty_since = 0;
    if (game && G().store.Dirty()) {
        if (!dirty_since) dirty_since = SDL_GetTicks();
        if (SDL_GetTicks() - dirty_since > 600 && !ImGui::IsAnyItemActive()) {
            if (!G().store.Save()) Toast(Tr(S::SaveFailed));
            dirty_since = 0;
        }
    } else {
        dirty_since = 0;
    }
    g.focus_page = false;
}

// ---------------------------------------------------------------------------------------------- start

// the launcher's own state for each port lives in the hub; the port's settings in its folder (a port not playable here
// gets no file of its folder)
void OpenGameFolder(Game& gm) {
    fs::path data = g.hub_dir / "dados" / FromU8(gm.entry.id);
    std::error_code ec;
    fs::create_directories(data, ec);
    bool here = Playable(gm, g.here);
    if (here) fs::create_directories(gm.entry.dir, ec);
    fs::path settings = here ? gm.entry.dir / FromU8(gm.prof.settings) : data / "settings.toml";
    fs::path ini = here && !gm.prof.ini.empty() ? gm.entry.dir / FromU8(gm.prof.ini) : fs::path();
    gm.store.Open(settings, data / "estado.toml", ini, gm.prof.ini_template);
    // a port's first settings: its first preset, the recommended one
    if (here && !fs::exists(settings, ec) && !gm.prof.presets.empty()) {
        for (auto& [k, v] : gm.prof.presets[0].values) {
            if (const Option* o = gm.prof.Find(k)) gm.store.Set(*o, v);
            else gm.store.SetKey(k, v);
        }
        gm.store.Save();
    }
}

void LoadGames() {
    for (const GameEntry& e : g.hub.games) {
        auto gm = std::make_unique<Game>();
        gm->entry = e;
        gm->ok = gm->prof.Load(e.profile, gm->error);
        if (!gm->ok && gm->error.empty()) gm->error = U8(e.profile);
        PlaceGame(*gm);
        OpenGameFolder(*gm);
        gm->art_spec = ReadArtSpec(e.profile);
        gm->theme_spec = ReadThemeSpec(e.profile);
        gm->icon_spec = ReadIconSpec(e.profile);
        StartArt(*gm);
        g.games.push_back(std::move(gm));
    }
    PinInstalledGames();
}

int FindGameIndex(const std::string& id) {
    for (size_t i = 0; i < g.games.size(); i++)
        if (g.games[i]->entry.id == id) return int(i);
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // Back + Start reaches us while a game runs
    // the window's and the taskbar's icon: the executable's (launcher.rc)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Otavix Launcher", SDL_GetError(), nullptr);
        return 1;
    }
    g.hub_dir = FromU8(SDL_GetBasePath());
    g.state.Open(g.hub_dir / "hub_state.toml");
    g.hub_ok = g.hub.Load(g.hub_dir, g.hub_error);
    SetGitHubProxy(g.hub.server_url);  // GitHub through the server: see GitHubApi
    std::string lang = g.state.Str("language", "auto");
    g.lang = lang == "pt" ? Lang::Pt : lang == "en" ? Lang::En : lang == "es" ? Lang::Es : SystemLang();

    g.content_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (g.content_scale <= 0) g.content_scale = 1;
    int ww = int(g.state.Int("window_w", 1280)), wh = int(g.state.Int("window_h", 800));
    g.win = SDL_CreateWindow(g.hub.name.c_str(), int(ww * g.content_scale), int(wh * g.content_scale),
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g.win) return 1;
    SDL_SetWindowMinimumSize(g.win, int(900 * g.content_scale), int(600 * g.content_scale));
    g.ren = SDL_CreateRenderer(g.win, nullptr);
    if (!g.ren) return 1;
    SDL_SetRenderVSync(g.ren, 1);
    SDL_SetWindowPosition(g.win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (g.state.Str("window_mode", "window") == "fullscreen") SDL_SetWindowFullscreen(g.win, true);
    SDL_ShowWindow(g.win);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ApplyScale();
    LoadFonts();
    ImGui_ImplSDL3_InitForSDLRenderer(g.win, g.ren);
    ImGui_ImplSDLRenderer3_Init(g.ren);
    ModesOfPrimary();
    LoadGames();
    g.music.Open();
    g.catalogs.SetFolder(g.hub_dir / "catalogo");
    g.covers.Open(g.ren, g.hub_dir / "cache" / "capas");
    g.music.SetVolume(float(g.state.Int("music_volume", 20)) / 100.0f);
    g.install_id = g.state.Str("install_id", "");
    if (g.install_id.size() != 32) {
        std::random_device rd;
        char id[33];
        for (int i = 0; i < 4; i++) std::snprintf(id + i * 8, 9, "%08x", rd());
        g.install_id = id;
        g.state.Set("install_id", g.install_id);
    }
    // the admin key: sealed; one still in plain text (an older launcher) gets sealed now
    g.admin_token = Unseal(g.state.Str("admin_key", ""));
    if (std::string plain = g.state.Str("admin_token", ""); !plain.empty()) {
        if (g.admin_token.empty()) g.admin_token = plain;
        std::string sealed = Seal(g.admin_token);
        if (!sealed.empty()) {
            g.state.Set("admin_key", sealed);
            g.state.Remove("admin_token");
        }
    }
    std::snprintf(g.admin_key, sizeof(g.admin_key), "%s", g.admin_token.c_str());
    g.art_t0 = SDL_GetTicks();


    // a game straight away: --jogo <id> (shortcuts), or the last one when the setting asks
    std::string open;
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--jogo") open = argv[i + 1];
    if (open.empty() && g.state.Bool("start_last", false)) open = g.state.Str("last_game", "");
    if (int i = FindGameIndex(open); i >= 0) OpenGame(i);
    // tests: LAUNCHER_ENTRY=n (the hub's sidebar), LAUNCHER_PAGE=n (the open game's), LAUNCHER_AUTOPLAY=1
    if (const char* e = SDL_getenv("LAUNCHER_ENTRY")) g.entry = std::clamp(std::atoi(e), 0, int(HubEntries().size()) - 1);
    // tests: LAUNCHER_SEARCH=text, the hub opens with that in the search bar and the first hit lit
    if (const char* q = SDL_getenv("LAUNCHER_SEARCH")) {
        SDL_strlcpy(g.search, q, sizeof(g.search));
        g.focus_page = true;
    }
    // the library's filter as it was left; tests: LAUNCHER_SEARCH_FILTER=n (0 all, 1 playable, 2 in development,
    // 3 installed)
    g.search_filter = int(std::clamp<int64_t>(g.state.Int("library_filter", 0), 0, 3));
    if (const char* f = SDL_getenv("LAUNCHER_SEARCH_FILTER")) g.search_filter = std::clamp(std::atoi(f), 0, 3);
    if (const char* p = SDL_getenv("LAUNCHER_PAGE"); p && g.cur >= 0) G().page = std::atoi(p);
    // tests: LAUNCHER_REPORT=text opens the open game's report page with that description, LAUNCHER_REPORT_PHOTO=file
    // adds a picture as the dialog would (LAUNCHER_REPORT_SEND=1 then presses Send, in ReportPage)
    if (const char* t = SDL_getenv("LAUNCHER_REPORT"); t && g.cur >= 0) {
        auto entries = Entries();
        for (size_t k = 0; k < entries.size(); k++)
            if (entries[k].kind == Kind::Report) G().page = int(k);
        SDL_strlcpy(g.rep_desc, t, sizeof(g.rep_desc));
    }
    if (const char* f = SDL_getenv("LAUNCHER_REPORT_PHOTO")) {
        std::lock_guard lock(g.dlg_m);
        g.rep_photo_picked.push_back(f);
    }
    // tests: LAUNCHER_SET="key=value;..." sets the open game's options (by their kind) and saves them
    if (const char* list = SDL_getenv("LAUNCHER_SET"); list && g.cur >= 0) {
        std::string all = list;
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(';', at), eq = all.find('=', at);
            if (end == std::string::npos) end = all.size();
            if (eq != std::string::npos && eq < end)
                if (const Option* o = G().prof.Find(all.substr(at, eq - at))) {
                    std::string v = all.substr(eq + 1, end - eq - 1);
                    Value val = v;
                    if (std::holds_alternative<int64_t>(o->def)) val = int64_t(std::atoll(v.c_str()));
                    else if (std::holds_alternative<bool>(o->def)) val = v == "1" || v == "true";
                    G().store.Set(*o, val);
                }
            at = end + 1;
        }
        G().store.Save();
    }
    // tests: LAUNCHER_PSX="package/feature=on;package/feature/option=value" on a PSXRecomp port's mods, then saved
    if (const char* list = SDL_getenv("LAUNCHER_PSX"); list && g.cur >= 0 && G().prof.mods_kind == "psxrecomp") {
        Game& gm = G();
        gm.psx_mods.Load(gm.entry.dir / FromU8(gm.prof.mods_folder), gm.prof.mods_game_id);
        std::string all = list;
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(';', at), eq = all.find('=', at);
            if (end == std::string::npos) end = all.size();
            std::string path = all.substr(at, eq - at), v = all.substr(eq + 1, end - eq - 1);
            for (const PsxPackage& pk : gm.psx_mods.Packages())
                if (path.rfind(pk.id + "/", 0) == 0) {
                    std::string rest = path.substr(pk.id.size() + 1);
                    for (const PsxFeature& f : pk.features)
                        if (rest == f.id) gm.psx_mods.SetEnabled(pk, f, v == "on");
                    for (const PsxOption& o : pk.options)
                        if (rest == o.feature + "/" + o.id) gm.psx_mods.Set(pk, o, v);
                }
            at = end + 1;
        }
        gm.psx_mods.Save();
    }
    // tests: LAUNCHER_INPUT_FETCH=key, the open game's install input's download button pressed
    if (const char* k = SDL_getenv("LAUNCHER_INPUT_FETCH"); k && g.cur >= 0)
        for (const BuildInput& b : G().prof.install.inputs)
            if (b.key == k) StartInputFetch(G(), b);
    // tests: LAUNCHER_SETTINGS_TAB=n, the launcher's settings open on that tab
    if (const char* t = SDL_getenv("LAUNCHER_SETTINGS_TAB")) {
        g.settings_tab = std::atoi(t);
        g.focus_tab = true;
    }
    if (SDL_getenv("LAUNCHER_AUTOPLAY") && g.cur >= 0) Launch(g.cur);
    // tests: LAUNCHER_FETCH=1 downloads the open game's port
    if (SDL_getenv("LAUNCHER_FETCH") && g.cur >= 0) G().fetcher.Install(G().entry.repo, G().entry.dir);
    // tests: LAUNCHER_GAME_FOLDER=folder, the open game's own folder picked (as Change folder... on its page)
    if (const char* f = SDL_getenv("LAUNCHER_GAME_FOLDER"); f && g.cur >= 0) MoveGameTo(G(), f);
    // tests: LAUNCHER_SHORTCUT=1 makes the open game's desktop shortcut
    if (SDL_getenv("LAUNCHER_SHORTCUT") && g.cur >= 0) MakeShortcut(G());
    // tests: LAUNCHER_INSTALL=file, installed into the open game as if picked in the dialog
    if (const char* f = SDL_getenv("LAUNCHER_INSTALL"); f && g.cur >= 0) {
        std::lock_guard lock(g.dlg_m);
        g.inst_path = f;
        g.inst_ready = true;
        g.inst_game = g.cur;
    }
    // tests: LAUNCHER_SEARCH=text, typed into the port request's game name
    if (const char* q = SDL_getenv("LAUNCHER_SEARCH")) std::snprintf(g.req_name, sizeof(g.req_name), "%s", q);
    // tests: LAUNCHER_PERF_TEST=file, a session of the open game measured as the file says, as if it had just closed
    if (const char* f = SDL_getenv("LAUNCHER_PERF_TEST"); f && g.cur >= 0) {
        g.perf_file = FromU8(f);
        G().last_minutes = 12;
        RecordSession(G());
    }
    if (g.state.Bool("check_updates", true) && !g.hub.update_repo.empty()) g.updater.Start(g.hub.update_repo, g.hub.version);

    while (!g.quit) {
        SDL_Event e;
        // while the game runs the launcher only waits for it
        if (g.proc && (SDL_GetWindowFlags(g.win) & SDL_WINDOW_MINIMIZED)) SDL_WaitEventTimeout(nullptr, 250);
        while (SDL_PollEvent(&e)) {
            if (CaptureEvent(e)) continue;
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) g.quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g.win)) g.quit = true;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11) {
                bool full = !(SDL_GetWindowFlags(g.win) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(g.win, full);
                g.state.Set("window_mode", std::string(full ? "fullscreen" : "window"));
            }
            if (e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED || e.type == SDL_EVENT_DISPLAY_ADDED) ModesOfPrimary();
        }
        PollGame();
        PeriodicChecks();
        BulkTick();
        PhotoTick();
        WatchCloseCombo();
        WatchIcon();
        TakeDialog();
        TakeArt();
        if (g.theme_made.exchange(false)) PlayTheme();
        ApplyInstalls();
        // tests: LAUNCHER_UPDATE=1 presses "Update now" as soon as a newer launcher is found
        if (static bool asked = SDL_getenv("LAUNCHER_UPDATE") != nullptr; asked) {
            UpdateCheck u = g.updater.Get();
            if (u.state == UpdateCheck::State::Newer) {
                g.updater.Download();
                asked = false;
            }
            if (u.state == UpdateCheck::State::Ready) UpdateActions(u);
        }
        g.covers.Take();
        if (SDL_GetWindowFlags(g.win) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        // the controller moves through the launcher only while its window has the focus and none of its games runs
        // (background events stay on for Back + Start, read on their own): a button pressed in another program, a
        // game the launcher did not open included, must not press one here
        static bool pad_on = true;
        bool want_pad = !g.proc && (SDL_GetWindowFlags(g.win) & SDL_WINDOW_INPUT_FOCUS);
        if (want_pad != pad_on) {
            pad_on = want_pad;
            if (want_pad) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoFirst);
            else ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
        }
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        Frame();
        ImGui::Render();
        SDL_SetRenderScale(g.ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(g.ren, 15, 15, 18, 255);
        SDL_RenderClear(g.ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g.ren);
        SDL_RenderPresent(g.ren);
    }
    for (auto& gp : g.games)
        if (gp->store.Dirty()) gp->store.Save();
    g.music.Close();
    g.covers.Close();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g.ren);
    SDL_DestroyWindow(g.win);
    SDL_Quit();
    // the art threads may still hold a game: leave without tearing the games down under them
    std::_Exit(0);
}

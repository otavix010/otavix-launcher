#include "closegame.h"

#include <set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#endif

namespace launcher {

namespace {

#ifdef _WIN32
// the process and everything it started, and what those started
std::set<DWORD> Tree(DWORD root) {
    std::set<DWORD> tree{root};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return tree;
    std::vector<std::pair<DWORD, DWORD>> all;  // pid, parent
    PROCESSENTRY32W e{sizeof e};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) all.push_back({e.th32ProcessID, e.th32ParentProcessID});
    CloseHandle(snap);
    for (bool grew = true; grew;) {
        grew = false;
        for (auto [pid, parent] : all)
            if (tree.count(parent) && tree.insert(pid).second) grew = true;
    }
    return tree;
}

struct Search {
    std::set<DWORD> tree;
    int found = 0;
    std::vector<void*> windows;
};

bool OwnWindow(const Search& s, HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return s.tree.count(pid) && IsWindowVisible(w) && !GetWindow(w, GW_OWNER);
}

BOOL CALLBACK Ask(HWND w, LPARAM p) {
    auto* s = reinterpret_cast<Search*>(p);
    if (OwnWindow(*s, w)) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        s->found++;
    }
    return TRUE;
}

BOOL CALLBACK Collect(HWND w, LPARAM p) {
    auto* s = reinterpret_cast<Search*>(p);
    if (OwnWindow(*s, w)) s->windows.push_back(w);
    return TRUE;
}
#else
// the process and everything it started, and what those started (parents read from /proc/<pid>/stat)
std::set<pid_t> Tree(pid_t root) {
    std::set<pid_t> tree{root};
    std::vector<std::pair<pid_t, pid_t>> all;  // pid, parent
    if (DIR* d = opendir("/proc")) {
        while (dirent* e = readdir(d)) {
            char* end = nullptr;
            const long pid = std::strtol(e->d_name, &end, 10);
            if (pid <= 0 || *end) continue;
            const std::string path = std::string("/proc/") + e->d_name + "/stat";
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) continue;
            char buf[512];
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            buf[n] = 0;
            // "pid (comm) state ppid ...": the name may hold spaces and parentheses, so read after the last ')'
            const char* close = std::strrchr(buf, ')');
            char state = 0;
            int parent = 0;
            if (close && std::sscanf(close + 1, " %c %d", &state, &parent) == 2) all.push_back({pid_t(pid), pid_t(parent)});
        }
        closedir(d);
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (auto [pid, parent] : all)
            if (tree.count(parent) && tree.insert(pid).second) grew = true;
    }
    return tree;
}

// the signal to every live process of the tree; how many got it
int SignalTree(pid_t root, int sig) {
    int sent = 0;
    for (pid_t p : Tree(root))
        if (kill(p, sig) == 0) sent++;
    return sent;
}
#endif

}  // namespace

bool AskToClose(uint64_t pid) {
#ifdef _WIN32
    Search s{Tree(DWORD(pid))};
    EnumWindows(Ask, reinterpret_cast<LPARAM>(&s));
    return s.found > 0;
#else
    // no window to post to from here (X11, Wayland, gamescope): SIGTERM is what closing asks of a Linux game; SDL
    // turns it into its quit event, so the game saves and exits as from its window's X
    return pid != 0 && SignalTree(pid_t(pid), SIGTERM) > 0;
#endif
}

std::vector<void*> GameWindows(uint64_t pid) {
#ifdef _WIN32
    Search s{Tree(DWORD(pid))};
    EnumWindows(Collect, reinterpret_cast<LPARAM>(&s));
    return s.windows;
#else
    (void)pid;
    return {};
#endif
}

void EndProcessTree(uint64_t pid) {
#ifdef _WIN32
    for (DWORD p : Tree(DWORD(pid))) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, p);
        if (!h) continue;
        TerminateProcess(h, 0);
        CloseHandle(h);
    }
#else
    if (pid != 0) SignalTree(pid_t(pid), SIGKILL);
#endif
}

}  // namespace launcher

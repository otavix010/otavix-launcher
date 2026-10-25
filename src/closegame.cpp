#include "closegame.h"

#include <set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
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
};

BOOL CALLBACK Ask(HWND w, LPARAM p) {
    auto* s = reinterpret_cast<Search*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (s->tree.count(pid) && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        s->found++;
    }
    return TRUE;
}
#endif

}  // namespace

bool AskToClose(uint64_t pid) {
#ifdef _WIN32
    Search s{Tree(DWORD(pid))};
    EnumWindows(Ask, reinterpret_cast<LPARAM>(&s));
    return s.found > 0;
#else
    (void)pid;
    return false;
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
    (void)pid;
#endif
}

}  // namespace launcher

#include "build.h"

#include <fstream>
#include <thread>

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace launcher {

namespace {

#ifdef _WIN32
std::wstring W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string N(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// a REG_EXPAND_SZ value, expanded
std::wstring RegPath(HKEY root, const wchar_t* key) {
    DWORD size = 0;
    if (RegGetValueW(root, key, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr, &size))
        return {};
    std::wstring v(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, v.data(), &size))
        return {};
    v.resize(wcsnlen(v.c_str(), v.size()));
    wchar_t out[32768];
    DWORD n = ExpandEnvironmentStringsW(v.c_str(), out, 32768);
    return n && n <= 32768 ? std::wstring(out) : v;
}

// this process's environment with PATH as a new terminal would get it: the machine's then the user's, from the
// registry, so a tool installed after the launcher started is found
std::wstring FreshEnvironment() {
    std::wstring path = RegPath(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    std::wstring user = RegPath(HKEY_CURRENT_USER, L"Environment");
    if (!user.empty()) path += L";" + user;
    std::wstring block;
    wchar_t* env = GetEnvironmentStringsW();
    for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        if (!path.empty() && _wcsnicmp(kv.c_str(), L"PATH=", 5) == 0) continue;
        block += kv;
        block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
    if (!path.empty()) {
        block += L"Path=" + path;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// one argument as the program reads it back (CommandLineToArgvW): backslashes doubled before a quote and at the end,
// or a folder ending with its separator ("F:\", "...\game\") came out as F:\" with the rest of the line in it
std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            slashes++;
        } else {
            if (c == L'"') q.append(slashes + 1, L'\\');
            slashes = 0;
        }
        q += c;
    }
    q.append(slashes, L'\\');
    return q + L"\"";
}

// starts cmd.exe /d /s /c "<line>" hidden, in the fresh environment, its output to the handle given
bool StartCmd(const std::string& line, HANDLE out, PROCESS_INFORMATION& pi, DWORD flags) {
    wchar_t cmd[MAX_PATH];
    GetSystemDirectoryW(cmd, MAX_PATH);
    std::wstring exe = std::wstring(cmd) + L"\\cmd.exe";
    std::wstring full = Quote(exe) + L" /d /s /c \"" + W(line) + L"\"";
    std::wstring env = FreshEnvironment();
    STARTUPINFOW si{sizeof si};
    if (out) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = out;
        si.hStdError = out;
    }
    return CreateProcessW(exe.c_str(), full.data(), nullptr, nullptr, out != nullptr, flags | CREATE_UNICODE_ENVIRONMENT,
                          env.data(), nullptr, &si, &pi);
}

std::string LastLine(const fs::path& log) {
    std::ifstream f(log, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    std::streamoff from = size > 4096 ? size - 4096 : 0;
    f.seekg(from);
    std::string tail(size_t(size - from), '\0');
    f.read(tail.data(), std::streamsize(tail.size()));
    std::string last;
    size_t at = 0;
    while (at < tail.size()) {
        size_t nl = tail.find('\n', at);
        std::string l = tail.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) last = l;
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return last.size() > 140 ? last.substr(0, 140) + "..." : last;
}

std::string Tail(const fs::path& log, int lines) {
    std::ifstream f(log, std::ios::binary);
    std::vector<std::string> all;
    for (std::string l; std::getline(f, l);) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) all.push_back(l);
    }
    std::string out;
    for (size_t i = all.size() > size_t(lines) ? all.size() - size_t(lines) : 0; i < all.size(); i++) out += all[i] + "\n";
    return out;
}
#endif

}  // namespace

std::string ExpandEnv(const std::string& s) {
#ifdef _WIN32
    if (s.find('%') == std::string::npos) return s;
    wchar_t out[32768];
    DWORD n = ExpandEnvironmentStringsW(W(s).c_str(), out, 32768);
    return n && n <= 32768 ? N(out) : s;
#else
    return s;
#endif
}

int RunHidden(const std::string& command_line, std::string* output, uint32_t timeout_ms) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) return -1;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi;
    bool ok = StartCmd(command_line, w, pi, CREATE_NO_WINDOW);
    CloseHandle(w);
    if (!ok) {
        CloseHandle(r);
        return -1;
    }
    std::string got;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(r, buf, sizeof buf, &n, nullptr) && n) got.append(buf, n);
    CloseHandle(r);
    DWORD code = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (output) *output = got;
    return int(code);
#else
    (void)command_line, (void)output, (void)timeout_ms;
    return -1;
#endif
}

bool RunInConsole(const std::string& command_line, const std::string& title) {
#ifdef _WIN32
    PROCESS_INFORMATION pi;
    std::string line = "title " + title + " & " + command_line + " & echo. & pause";
    if (!StartCmd(line, nullptr, pi, CREATE_NEW_CONSOLE)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)command_line, (void)title;
    return false;
#endif
}

void GameBuild::Start(const std::vector<std::string>& args, const fs::path& marks, const std::vector<std::string>& steps) {
    {
        std::lock_guard lock(m_);
        if (st_.stage == Stage::Running) return;
        st_ = {};
        st_.stage = Stage::Running;
        st_.started = SDL_GetTicks();
    }
#ifdef _WIN32
    std::thread([this, args, marks, steps] {
        auto fail = [&](const std::string& why, int code) {
            std::lock_guard lock(m_);
            st_.stage = Stage::Failed;
            st_.error = why;
            st_.code = code;
        };
        std::error_code ec;
        fs::create_directories(marks, ec);
        fs::path log = marks / "launcher.log";
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) return fail("não foi possível criar " + U8(log), -1);
        // UTF-8 in the log: the console's code page would garble the generator's accents
        std::string line = "chcp 65001 >nul &";
        for (const std::string& a : args) line += " " + N(Quote(W(ExpandEnv(a))));
        // the tree in a job: a cancel ends the generator and everything it started (compilers, python)
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        PROCESS_INFORMATION pi;
        bool ok = StartCmd(line, out, pi, CREATE_NO_WINDOW | CREATE_SUSPENDED);
        CloseHandle(out);
        if (!ok) {
            if (job) CloseHandle(job);
            return fail("não foi possível iniciar o gerador", -1);
        }
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        {
            std::lock_guard lock(m_);
            job_ = job;
        }
        // progress: the steps whose mark is there, the current one's newest log line. The step's log is opened as
        // little as possible, every 2 s and only once it grew: Windows PowerShell's Add-Content and Set-Content fail
        // ("being used by another process") while any other process has the file open, even only reading it, and
        // a generator writing its step log with them would fail the step
        std::string last;
        fs::path step_log;
        uintmax_t step_size = 0;
        for (int tick = 0; WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT; tick++) {
            int done = 0;
            while (done < int(steps.size()) && fs::exists(marks / (steps[size_t(done)] + ".ok"), ec)) done++;
            fs::path cur = done < int(steps.size()) ? marks / (steps[size_t(done)] + ".log") : fs::path();
            if (cur != step_log) step_log = cur, step_size = 0, last.clear();
            if (tick % 4 == 0) {
                uintmax_t size = step_log.empty() ? 0 : fs::file_size(step_log, ec);
                if (ec) size = 0;
                if (size != step_size) step_size = size, last = LastLine(step_log);
                if (last.empty()) last = LastLine(log);  // the generator's own messages, through the handle it inherited
            }
            std::lock_guard lock(m_);
            st_.step = done;
            st_.line = last;
        }
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        {
            std::lock_guard lock(m_);
            job_ = nullptr;
        }
        if (job) CloseHandle(job);
        int done = 0;
        while (done < int(steps.size()) && fs::exists(marks / (steps[size_t(done)] + ".ok"), ec)) done++;
        if (code != 0 || done < int(steps.size())) return fail(Tail(log, 10), int(code));
        std::lock_guard lock(m_);
        st_.stage = Stage::Done;
        st_.step = done;
    }).detach();
#else
    (void)args, (void)marks, (void)steps;
    std::lock_guard lock(m_);
    st_.stage = Stage::Failed;
    st_.error = "not on this system yet";
#endif
}

void GameBuild::Cancel() {
#ifdef _WIN32
    std::lock_guard lock(m_);
    if (job_) TerminateJobObject(static_cast<HANDLE>(job_), 1);
#endif
}

GameBuild::Status GameBuild::Get() {
    std::lock_guard lock(m_);
    return st_;
}

}  // namespace launcher

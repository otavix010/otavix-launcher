// A port made on the player's PC ([install] kind = "build"): its generator turns the player's copy of the game into
// the executable. It runs hidden, its output in a log; its steps leave <id>.ok marks, which tell the progress and let a
// stopped run carry on. Tools it needs are checked first, each with the command that installs it.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "profile.h"

namespace launcher {

// %VAR% replaced by the environment's values
std::string ExpandEnv(const std::string& s);

// runs a command line hidden, through cmd so PATH is the one a new terminal would have (a tool just installed is
// found), and waits for it: its exit code, -1 when it could not start or took longer than the limit
int RunHidden(const std::string& command_line, std::string* output = nullptr, uint32_t timeout_ms = 30000);
// opens a console that runs the command line and stays open (an installer the player watches)
bool RunInConsole(const std::string& command_line, const std::string& title);

class GameBuild {
public:
    enum class Stage { Idle, Running, Done, Failed };
    struct Status {
        Stage stage = Stage::Idle;
        int step = 0;             // steps done
        uint64_t started = 0;     // SDL ticks
        std::string line;         // the newest line of the current step's log
        std::string error;        // the end of the log when it failed
        int code = 0;
    };
    // args: the generator and its arguments; marks: the folder of the steps' .ok and .log files
    void Start(const std::vector<std::string>& args, const fs::path& marks, const std::vector<std::string>& steps);
    void Cancel();
    Status Get();
    bool Busy() { return Get().stage == Stage::Running; }

private:
    std::mutex m_;
    Status st_;
    void* job_ = nullptr;  // the process tree, ended together on cancel
};

}  // namespace launcher

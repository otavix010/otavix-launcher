// Updates: the newest release of the project on GitHub against the installed version. Checking runs on a thread;
// installing opens the release's page until the project publishes packages the launcher can apply itself.
#pragma once
#include <mutex>
#include <string>

namespace launcher {

struct UpdateCheck {
    enum class State { Idle, Checking, UpToDate, Newer, NoRepo, Failed };
    State state = State::Idle;
    std::string latest;  // the release's tag
    std::string url;     // its page
    std::string error;
};

class Updater {
public:
    void Start(const std::string& repo, const std::string& installed);  // a check on a thread of its own
    UpdateCheck Get();

private:
    std::mutex m_;
    UpdateCheck cur_;
};

// "v1.2.10" against "1.2.9": numbers compared in turn
bool NewerVersion(const std::string& latest, const std::string& installed);

}  // namespace launcher

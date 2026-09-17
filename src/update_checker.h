#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

struct UpdateCheckResult {
    bool succeeded = false;
    bool updateAvailable = false;
    std::wstring latestVersion;
    std::wstring releaseUrl;
    std::wstring message;
};

class UpdateChecker {
public:
    ~UpdateChecker();
    bool Start(HWND window, UINT completionMessage);
    void Shutdown();
    bool Active() const { return active_.load(); }

private:
    std::thread worker_;
    std::atomic<bool> active_{ false };
    std::atomic<bool> shuttingDown_{ false };
};

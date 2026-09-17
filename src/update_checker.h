#pragma once

#include <windows.h>

#include <atomic>
#include <array>
#include <string>
#include <thread>

struct UpdateCheckResult {
    bool succeeded = false;
    bool updateAvailable = false;
    std::wstring latestVersion;
    std::wstring releaseUrl;
    std::wstring message;
};

bool ParseUpdateVersion(const std::wstring& text, std::array<unsigned int, 4>& version);

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

#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>

class AutostartManager {
public:
    static AutostartManager& Get();

    // Check if autorun is enabled (checks Task Scheduler and Registry)
    bool IsAutostartEnabled();

    // Enable or disable autorun with Windows (Task Scheduler + HKCU Run)
    bool SetAutostart(bool enable);
    bool EnableAutostart() { return SetAutostart(true); }

    // Immediately disable autorun and purge all autostart entries (Task Scheduler + HKCU/HKLM Run/RunOnce)
    bool DisableAutostart();

    // Sync task state with config on application launch
    void SyncStartupState();

private:
    AutostartManager() = default;
    ~AutostartManager() = default;

    std::wstring GetExecutablePath() const;
    bool RunSilentCommand(const std::wstring& cmd);

    std::atomic<bool> m_cachedAutostart{false};
    std::atomic<bool> m_initialized{false};
    std::mutex m_mutex;
};

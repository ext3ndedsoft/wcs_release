#pragma once

#include <string>
#include <functional>
#include <atomic>

class RestoreManager {
public:
    static RestoreManager& Get();

    // Create native Windows System Restore Point (Synchronous)
    bool CreateSystemRestorePoint(const std::wstring& description, std::string& outMessage);

    // Create native Windows System Restore Point (Asynchronous, non-blocking)
    void CreateSystemRestorePointAsync(const std::wstring& description, std::function<void(bool success, const std::string& msg)> onComplete);

    bool IsBusy() const { return m_isBusy.load(); }
    std::string GetBusyStatus() const { return m_busyStatus; }

    // Save/Load WCS settings profile to JSON/INI file
    bool ExportProfile(const std::wstring& filePath, std::string& outMessage);
    bool ImportProfile(const std::wstring& filePath, std::string& outMessage);

private:
    RestoreManager() = default;
    ~RestoreManager() = default;

    std::atomic<bool> m_isBusy{ false };
    std::string m_busyStatus;
};

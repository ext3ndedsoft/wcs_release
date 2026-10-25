#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include "third_party/json.hpp"

struct StartupItem {
    enum class LocationType {
        HKCU_Run,
        HKLM_Run,
        HKLM_Run32,
        Folder_User,
        Folder_Common
    };

    std::string id;
    std::string name;
    std::string command;
    std::string location; // "HKCU Run", "HKLM Run", "HKLM (32-bit)", "Startup Folder (User)", "Startup Folder (All Users)"
    bool enabled = true;
    std::string publisher;
    bool exists = true;

    // Internal details for modification and removal
    LocationType locType = LocationType::HKCU_Run;
    std::wstring regValueName;
    std::wstring filePath; // For folder items
};

class StartupManager {
public:
    static StartupManager& Get();

    void Init();
    void ScanItems();
    nlohmann::json GetItemsJson();
    nlohmann::json ToggleItem(const std::string& id, bool enabled);
    nlohmann::json DeleteItem(const std::string& id);

private:
    StartupManager();
    ~StartupManager() = default;
    StartupManager(const StartupManager&) = delete;
    StartupManager& operator=(const StartupManager&) = delete;

    void ScanItemsLocked();
    void ScanRegistryKey(HKEY hRoot, const std::wstring& subKey, const std::wstring& approvedSubKey,
                         StartupItem::LocationType locType, const std::string& locStr, DWORD extraSam = 0);
    void ScanFolder(const std::wstring& folderPath, StartupItem::LocationType locType, const std::string& locStr);

    std::mutex m_mutex;
    std::vector<StartupItem> m_items;
    bool m_initialized = false;
};

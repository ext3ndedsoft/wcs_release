#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include "third_party/json.hpp"

struct PciMsiDevice {
    std::string id;            // Unique device key path / instance ID
    std::string name;          // Friendly device name
    std::string category;      // "GPU" | "Network" | "USB" | "Audio" | "Storage" | "Other"
    bool msiSupported = false; // Does the hardware capability / key exist
    bool msiEnabled = false;   // MSISupported == 1
    int messageLimit = 1;      // MessageNumberLimit
    int priority = 0;          // 0 = Undefined/Default, 1 = Low, 2 = Normal, 3 = High
    bool canModify = true;     // True if non-critical device
    std::string location;      // LocationInformation / Bus Dev Func
    std::string hardwareId;    // HardwareID
};

class MsiManager {
public:
    static MsiManager& Get();

    void ScanDevices();
    nlohmann::json GetDevicesJson();
    bool SetMsiEnabled(const std::string& id, bool enabled, std::string& outMsg);
    bool SetDevicePriority(const std::string& id, int priority, std::string& outMsg);
    bool ApplyRecommendedEsportsProfile(std::string& outMsg);
    bool IsCategoryMsiEnabled(const std::string& category);
    bool SetCategoryMsi(const std::string& category, bool enabled);

private:
    MsiManager();
    ~MsiManager() = default;
    MsiManager(const MsiManager&) = delete;
    MsiManager& operator=(const MsiManager&) = delete;

    void ScanDevicesLocked();

    std::mutex m_mutex;
    std::vector<PciMsiDevice> m_devices;
    bool m_scanned = false;
};

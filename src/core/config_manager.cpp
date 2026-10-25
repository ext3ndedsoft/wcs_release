#include "config_manager.h"
#include "third_party/json.hpp"
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <iostream>

ConfigManager& ConfigManager::Get() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() {
    // Default esports settings
    m_config.activePreset = 1; // Esports Ultra
    m_config.runInBackground = true;
    m_config.startMinimizedToTray = false;
    m_config.autostartWithWindows = false;
    m_config.autoRamCleanEnabled = true;
    m_config.trimWorkingSetOnThreshold = false; // Safe: no stutter in games from page fault spikes
    m_config.clearCacheOnThreshold = true;      // Safe: standby cache purge (ISLC method)
    m_config.ramThresholdPercent = 85;
    m_config.trimWorkingSetOnInterval = false;  // Never automatically evict working sets of running games
    m_config.clearCacheOnInterval = false;
    m_config.ramCleanIntervalMinutes = 30;
    m_config.gameBoostEnabled = true;
    m_config.boostHighPriority = true;
    m_config.boostStandbyPurge = true;
    m_config.boostLockTimer = true;
    m_config.customBoostGames = {};
    m_config.autoCheckUpdates = true;
    m_config.updateChannel = "@wcs_releases";
}

std::string ConfigManager::GetConfigFilePath() const {
    char* localAppData = nullptr;
    size_t len = 0;
    if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData != nullptr) {
        std::filesystem::path dir = std::filesystem::path(localAppData) / "WindowsConfigStudio";
        free(localAppData);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!ec) {
            return (dir / "wcs_config.json").string();
        }
    }

    // Fallback next to exe
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    return (dir / "wcs_config.json").string();
}

bool ConfigManager::Load() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string path = GetConfigFilePath();
    
    // Check primary path first, fallback next to exe if not found in LocalAppData
    if (!std::filesystem::exists(path)) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::string fallbackPath = (exeDir / "wcs_config.json").string();
        if (std::filesystem::exists(fallbackPath)) {
            path = fallbackPath;
        }
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        Save(); // Create default config file
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (...) {
        file.close();
        Save();
        return false;
    }
    file.close();

    m_config.configVersion = j.value("configVersion", 1);
    m_config.activePreset = j.value("activePreset", m_config.activePreset);
    m_config.runInBackground = j.value("runInBackground", m_config.runInBackground);
    m_config.startMinimizedToTray = j.value("startMinimizedToTray", m_config.startMinimizedToTray);
    m_config.autostartWithWindows = j.value("autostartWithWindows", m_config.autostartWithWindows);
    m_config.autoRamCleanEnabled = j.value("autoRamCleanEnabled", m_config.autoRamCleanEnabled);

    if (m_config.configVersion < 2) {
        // Automatically upgrade legacy config: disable aggressive working-set trimming
        // which forces page-outs and hard page-fault stutters in games
        m_config.trimWorkingSetOnInterval = false;
        m_config.trimWorkingSetOnThreshold = false;
        m_config.clearCacheOnThreshold = true;
        m_config.configVersion = 2;
    } else {
        m_config.trimWorkingSetOnThreshold = j.value("trimWorkingSetOnThreshold", m_config.trimWorkingSetOnThreshold);
        m_config.trimWorkingSetOnInterval = j.value("trimWorkingSetOnInterval", m_config.trimWorkingSetOnInterval);
    }

    m_config.clearCacheOnThreshold = j.value("clearCacheOnThreshold", m_config.clearCacheOnThreshold);
    m_config.ramThresholdPercent = j.value("ramThresholdPercent", m_config.ramThresholdPercent);
    m_config.clearCacheOnInterval = j.value("clearCacheOnInterval", m_config.clearCacheOnInterval);
    m_config.ramCleanIntervalMinutes = j.value("ramCleanIntervalMinutes", m_config.ramCleanIntervalMinutes);
    if (m_config.ramCleanIntervalMinutes < 1) m_config.ramCleanIntervalMinutes = 15;

    if (j.contains("themeAccent") && j["themeAccent"].is_string()) {
        m_config.themeAccent = j["themeAccent"].get<std::string>();
    }

    m_config.gameBoostEnabled = j.value("gameBoostEnabled", m_config.gameBoostEnabled);
    m_config.boostHighPriority = j.value("boostHighPriority", m_config.boostHighPriority);
    m_config.boostStandbyPurge = j.value("boostStandbyPurge", m_config.boostStandbyPurge);
    m_config.boostLockTimer = j.value("boostLockTimer", m_config.boostLockTimer);
    if (j.contains("customBoostGames") && j["customBoostGames"].is_array()) {
        m_config.customBoostGames = j["customBoostGames"].get<std::vector<std::string>>();
    }
    if (j.contains("ignoredBoostGames") && j["ignoredBoostGames"].is_array()) {
        m_config.ignoredBoostGames = j["ignoredBoostGames"].get<std::vector<std::string>>();
    }

    m_config.autoCheckUpdates = j.value("autoCheckUpdates", m_config.autoCheckUpdates);
    if (j.contains("updateChannel") && j["updateChannel"].is_string()) {
        m_config.updateChannel = j["updateChannel"].get<std::string>();
    }

    if (j.contains("tweaks") && j["tweaks"].is_object()) {
        const auto& tj = j["tweaks"];
        auto& t = m_config.tweaks;
        t.timerResolution05ms = tj.value("timerResolution05ms", t.timerResolution05ms);
        t.disableDynamicTick  = tj.value("disableDynamicTick", t.disableDynamicTick);
        t.usePlatformClockNo  = tj.value("usePlatformClockNo", t.usePlatformClockNo);
        t.usePlatformTickNo   = tj.value("usePlatformTickNo", t.usePlatformTickNo);
        t.win32PrioritySeparation = tj.value("win32PrioritySeparation", t.win32PrioritySeparation);
        t.win32PriorityValue  = (DWORD)tj.value("win32PriorityValue", (int)t.win32PriorityValue);
        t.ultimatePowerPlan   = tj.value("ultimatePowerPlan", t.ultimatePowerPlan);
        t.disableCoreParking  = tj.value("disableCoreParking", t.disableCoreParking);

        t.gpuMsiMode          = tj.value("gpuMsiMode", t.gpuMsiMode);
        t.audioMsiMode        = tj.value("audioMsiMode", t.audioMsiMode);
        t.hagsEnabled         = tj.value("hagsEnabled", t.hagsEnabled);
        t.csrssDwmPriority    = tj.value("csrssDwmPriority", t.csrssDwmPriority);
        t.disableGameDVR      = tj.value("disableGameDVR", t.disableGameDVR);
        t.disableFullscreenOptimizations = tj.value("disableFullscreenOptimizations", t.disableFullscreenOptimizations);

        t.mmcssGamesPriority  = tj.value("mmcssGamesPriority", t.mmcssGamesPriority);
        t.disableNetworkThrottling = tj.value("disableNetworkThrottling", t.disableNetworkThrottling);

        t.tcpNoDelay          = tj.value("tcpNoDelay", t.tcpNoDelay);
        t.tcpAutoTuning       = tj.value("tcpAutoTuning", t.tcpAutoTuning);

        t.disablePagingExecutive = tj.value("disablePagingExecutive", t.disablePagingExecutive);
        t.largeSystemCacheDisabled = tj.value("largeSystemCacheDisabled", t.largeSystemCacheDisabled);

        t.flatMouseCurve      = tj.value("flatMouseCurve", t.flatMouseCurve);
        t.keyboardMaxSpeed    = tj.value("keyboardMaxSpeed", t.keyboardMaxSpeed);
        t.disableTelemetry    = tj.value("disableTelemetry", t.disableTelemetry);
        t.disableErrorReporting = tj.value("disableErrorReporting", t.disableErrorReporting);
    }

    return true;
}

bool ConfigManager::Save() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string path = GetConfigFilePath();
    
    // Ensure parent directory exists
    std::filesystem::path parentDir = std::filesystem::path(path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parentDir, ec);

    std::ofstream file(path);
    if (!file.is_open()) return false;

    const auto& t = m_config.tweaks;

    nlohmann::json j;
    j["configVersion"] = m_config.configVersion;
    j["activePreset"] = m_config.activePreset;
    j["runInBackground"] = m_config.runInBackground;
    j["startMinimizedToTray"] = m_config.startMinimizedToTray;
    j["autostartWithWindows"] = m_config.autostartWithWindows;
    j["autoRamCleanEnabled"] = m_config.autoRamCleanEnabled;
    j["trimWorkingSetOnThreshold"] = m_config.trimWorkingSetOnThreshold;
    j["clearCacheOnThreshold"] = m_config.clearCacheOnThreshold;
    j["ramThresholdPercent"] = m_config.ramThresholdPercent;
    j["trimWorkingSetOnInterval"] = m_config.trimWorkingSetOnInterval;
    j["clearCacheOnInterval"] = m_config.clearCacheOnInterval;
    j["ramCleanIntervalMinutes"] = m_config.ramCleanIntervalMinutes;
    j["themeAccent"] = m_config.themeAccent;
    j["gameBoostEnabled"] = m_config.gameBoostEnabled;
    j["boostHighPriority"] = m_config.boostHighPriority;
    j["boostStandbyPurge"] = m_config.boostStandbyPurge;
    j["boostLockTimer"] = m_config.boostLockTimer;
    j["customBoostGames"] = m_config.customBoostGames;
    j["ignoredBoostGames"] = m_config.ignoredBoostGames;
    j["autoCheckUpdates"] = m_config.autoCheckUpdates;
    j["updateChannel"] = m_config.updateChannel;

    j["tweaks"] = {
        { "timerResolution05ms", t.timerResolution05ms },
        { "disableDynamicTick", t.disableDynamicTick },
        { "usePlatformClockNo", t.usePlatformClockNo },
        { "usePlatformTickNo", t.usePlatformTickNo },
        { "win32PrioritySeparation", t.win32PrioritySeparation },
        { "win32PriorityValue", t.win32PriorityValue },
        { "ultimatePowerPlan", t.ultimatePowerPlan },
        { "disableCoreParking", t.disableCoreParking },
        { "gpuMsiMode", t.gpuMsiMode },
        { "audioMsiMode", t.audioMsiMode },
        { "hagsEnabled", t.hagsEnabled },
        { "csrssDwmPriority", t.csrssDwmPriority },
        { "disableGameDVR", t.disableGameDVR },
        { "disableFullscreenOptimizations", t.disableFullscreenOptimizations },
        { "mmcssGamesPriority", t.mmcssGamesPriority },
        { "disableNetworkThrottling", t.disableNetworkThrottling },
        { "tcpNoDelay", t.tcpNoDelay },
        { "tcpAutoTuning", t.tcpAutoTuning },
        { "disablePagingExecutive", t.disablePagingExecutive },
        { "largeSystemCacheDisabled", t.largeSystemCacheDisabled },
        { "flatMouseCurve", t.flatMouseCurve },
        { "keyboardMaxSpeed", t.keyboardMaxSpeed },
        { "disableTelemetry", t.disableTelemetry },
        { "disableErrorReporting", t.disableErrorReporting }
    };

    file << j.dump(2);
    file.close();
    return true;
}

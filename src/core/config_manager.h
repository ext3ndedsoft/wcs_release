#pragma once

#include <string>
#include <vector>
#include "tweaks_engine.h"

struct AppConfig {
    // Tweaks State
    TweakState tweaks;
    int activePreset = 0; // 0: None, 1: EsportsUltra, 2: Balanced, 3: Default

    // Background & Tray
    bool runInBackground = true;
    bool startMinimizedToTray = false;
    bool autostartWithWindows = false;

    int configVersion = 2;

    // RAM Cleaner (Memory Cleaner Configurator)
    bool autoRamCleanEnabled = true;
    bool trimWorkingSetOnThreshold = false; // Safe: disabled by default to avoid game stuttering
    bool clearCacheOnThreshold = true;      // Safe: standby list cache purge (ISLC method)
    int ramThresholdPercent = 85;           // 60 to 95%
    bool trimWorkingSetOnInterval = false;  // Safe: disabled by default
    bool clearCacheOnInterval = false;
    int ramCleanIntervalMinutes = 30;       // 3 to 120 minutes

    // Theme Accent
    std::string themeAccent = "cyan"; // "cyan", "emerald", "crimson", "violet", "amber"

    // Game Detection & Dynamic Boost
    bool gameBoostEnabled = true;
    bool boostHighPriority = true;
    bool boostStandbyPurge = true;
    bool boostLockTimer = true;
    std::vector<std::string> customBoostGames;
    std::vector<std::string> ignoredBoostGames;

    // In-App Updates & Releases Feed
    bool autoCheckUpdates = true;
    std::string updateChannel = "https://github.com/ext3ndedsoft/wcs_release";
};

#include <mutex>

class ConfigManager {
public:
    static ConfigManager& Get();

    bool Load();
    bool Save();

    AppConfig& GetConfig() { return m_config; }
    const AppConfig& GetConfig() const { return m_config; }

    std::recursive_mutex& GetMutex() const { return m_mutex; }
    std::string GetConfigFilePath() const;

private:
    ConfigManager();
    ~ConfigManager() = default;

    mutable std::recursive_mutex m_mutex;
    AppConfig m_config;
};

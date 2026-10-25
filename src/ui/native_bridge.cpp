#include "native_bridge.h"
#include "tweaks_engine.h"
#include "system_tweaker.h"
#include "system_tweaks.h"
#include "memory_cleaner.h"
#include "timer_resolution.h"
#include "system_info.h"
#include "restore_manager.h"
#include "config_manager.h"
#include "autostart_manager.h"
#include "win11_tweaker.h"
#include "startup_manager.h"
#include "msi_manager.h"
#include "game_boost_manager.h"
#include "update_manager.h"
#include "debug_logger.h"
#include "common_utils.h"

#include <commdlg.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>

NativeBridge& NativeBridge::Get() {
    static NativeBridge instance;
    return instance;
}

NativeBridge::NativeBridge() {
}

void NativeBridge::RegisterWebView(ICoreWebView2* webView, bool isMain) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!webView) return;
    webView->AddRef();
    m_webViews.push_back(webView);
    if (isMain) {
        m_mainWebView = webView;
    }
}

void NativeBridge::UnregisterWebView(ICoreWebView2* webView) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_webViews.begin(); it != m_webViews.end(); ) {
        if (*it == webView) {
            (*it)->Release();
            it = m_webViews.erase(it);
        } else {
            ++it;
        }
    }
    if (m_mainWebView == webView) {
        m_mainWebView = nullptr;
    }
}

void NativeBridge::SetMainHwnd(HWND hWnd) {
    m_mainHwnd = hWnd;
    if (hWnd) {
        m_mainThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    }
}

void NativeBridge::DispatchToUI(std::function<void()> task) {
    if (!task) return;
    if (m_mainThreadId != 0 && GetCurrentThreadId() == m_mainThreadId) {
        task();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_uiTaskMutex);
        m_uiTasks.push_back(std::move(task));
    }
    if (m_mainHwnd) {
        PostMessageW(m_mainHwnd, WM_USER + 150, 0, 0);
    }
}

void NativeBridge::ProcessUITasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_uiTaskMutex);
        tasks.swap(m_uiTasks);
    }
    for (auto& t : tasks) {
        if (t) {
            try { t(); } catch (...) {}
        }
    }
}

void NativeBridge::BroadcastEvent(const std::string& eventName, const nlohmann::json& payload) {
    nlohmann::json msg;
    msg["event"] = eventName;
    msg["data"] = payload;
    std::string jsonStr = msg.dump();
    std::wstring jsonW = Utf8ToWide(jsonStr);

    DispatchToUI([this, jsonW]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto* wv : m_webViews) {
            if (wv) {
                wv->PostWebMessageAsJson(jsonW.c_str());
            }
        }
    });
}

void NativeBridge::ShowToast(const std::string& message, bool isError) {
    nlohmann::json toast;
    toast["message"] = message;
    toast["isError"] = isError;
    BroadcastEvent("toast", toast);
}

void NativeBridge::BroadcastTelemetry() {
    float timerMs = SystemInfo::Get().QueryLiveTimerResolution();
    auto memStats = MemoryCleaner::Get().QueryDetailedStats(true);
    bool isLockActive = TimerResolutionService::Get().IsActive();

    float cpuPercent = SystemInfo::Get().QueryLiveCpuUsage();
    float gpuPercent = SystemInfo::Get().QueryLiveGpuUsage();
    auto specs = SystemInfo::Get().GetSpecs();

    nlohmann::json data;
    data["timerMs"] = timerMs;
    data["timerLockActive"] = isLockActive;
    data["cpuPercent"] = cpuPercent;
    data["gpuPercent"] = gpuPercent;
    data["cpuName"] = specs.cpuName;
    data["gpuName"] = specs.primaryGPUName;
    data["logicalCores"] = specs.logicalCores;
    data["ramUsedGb"] = memStats.usedRAM_GB;
    data["ramTotalGb"] = memStats.totalRAM_GB;
    data["ramFreeGb"] = memStats.freeRAM_GB;
    data["ramStandbyGb"] = memStats.standbyRAM_GB;
    data["ramPercent"] = memStats.usedPercent;
    data["pagefileUsedGb"] = memStats.pagefileUsed_GB;
    data["pagefileTotalGb"] = memStats.pagefileTotal_GB;
    data["pagefilePercent"] = memStats.pagefilePercent;
    data["peakMemoryPercent"] = memStats.peakMemoryPercent;
    data["remainingCleanSec"] = MemoryCleaner::Get().GetRemainingSeconds();
    data["lastFreedBytes"] = MemoryCleaner::Get().GetLastFreedBytes();
    data["lastFreedStr"] = MemoryCleaner::FormatBytes(MemoryCleaner::Get().GetLastFreedBytes());
    data["lastActionStatus"] = MemoryCleaner::Get().GetLastActionStatus();

    BroadcastEvent("telemetry_update", data);
}

void NativeBridge::HandleWebMessage(ICoreWebView2* sender, const std::wstring& messageW) {
    std::string messageUtf8 = WideToUtf8(messageW);
    try {
        nlohmann::json req = nlohmann::json::parse(messageUtf8);
        int64_t reqId = req.value("id", (int64_t)0);
        std::string action = req.value("action", "");
        nlohmann::json params = req.value("params", nlohmann::json::object());

        auto isHeavy = [&action]() -> bool {
            return action == "applyPreset" ||
                   action == "applyAllTweaks" ||
                   action == "revertToDefaults" ||
                   action == "executeCleanup" ||
                   action == "deepCleanAction" ||
                   action == "getStartupItems" ||
                   action == "scanStartup" ||
                   action == "getMsiDevices" ||
                   action == "scanDevices" ||
                   action == "scanMsiDevices" ||
                   action == "applyMsiEsportsProfile" ||
                   action == "checkUpdates" ||
                   action == "scanAppx" ||
                   action == "createRestorePoint" ||
                   action == "cleanAllMemory" ||
                   action == "purgeStandbyMemory" ||
                   action == "clearSystemCache" ||
                   action == "trimWorkingSet" ||
                   action == "applySystemTweaks" ||
                   action == "applySecurityTweaks" ||
                   action == "applyAllOsPerfSubTweaks" ||
                   action == "applyAllSsdPerfSubTweaks" ||
                   action == "applyAllTelemetrySubTweaks" ||
                   action == "applyAllContextMenuTweaks" ||
                   action == "scanInstalledGames" ||
                   action == "fetchGameCover" ||
                   action == "flushDNS";
        };

        if (isHeavy()) {
            if (sender) sender->AddRef();
            std::thread([this, sender, reqId, action, params]() {
                try {
                    nlohmann::json result;
                    try {
                        result = ProcessAction(action, params);
                    } catch (const std::exception& e) {
                        result = nlohmann::json::object();
                        result["success"] = false;
                        result["error"] = e.what();
                    } catch (...) {
                        result = nlohmann::json::object();
                        result["success"] = false;
                        result["error"] = "Native operation failed";
                    }

                    // Completion broadcast event notification
                    if (action == "executeCleanup") {
                        BroadcastEvent("cleanup_done", result);
                    } else if (action == "applyPreset") {
                        BroadcastEvent("preset_applied", result);
                    } else if (action == "getMsiDevices" || action == "scanDevices" || action == "scanMsiDevices") {
                        BroadcastEvent("msi_devices_scanned", result);
                    } else if (action == "getStartupItems" || action == "scanStartup") {
                        BroadcastEvent("startup_items_scanned", result);
                    } else if (action == "checkUpdates") {
                        if (result.is_object() && result.value("available", false)) {
                            BroadcastEvent("update_available", result);
                        }
                        BroadcastEvent("updates_checked", result);
                    } else if (action == "scanAppx") {
                        BroadcastEvent("appx_scanned", result);
                    }

                    nlohmann::json resp;
                    resp["id"] = reqId;
                    resp["action"] = action + "_response";
                    resp["success"] = result.is_object() ? result.value("success", true) : true;
                    resp["data"] = result;

                    std::string respStr = resp.dump();
                    std::wstring respW = Utf8ToWide(respStr);

                    DispatchToUI([this, sender, respW]() {
                        try {
                            bool stillValid = false;
                            {
                                std::lock_guard<std::mutex> lock(m_mutex);
                                for (auto* wv : m_webViews) {
                                    if (wv == sender) {
                                        stillValid = true;
                                        break;
                                    }
                                }
                            }
                            if (stillValid && sender) {
                                sender->PostWebMessageAsJson(respW.c_str());
                            }
                        } catch (...) {}
                        if (sender) {
                            sender->Release();
                        }
                    });
                } catch (...) {
                    if (sender) {
                        sender->Release();
                    }
                }
            }).detach();
            return;
        }

        nlohmann::json result = ProcessAction(action, params);

        nlohmann::json resp;
        resp["id"] = reqId;
        resp["action"] = action + "_response";
        resp["success"] = result.is_object() ? result.value("success", true) : true;
        resp["data"] = result;

        std::string respStr = resp.dump();
        std::wstring respW = Utf8ToWide(respStr);
        sender->PostWebMessageAsJson(respW.c_str());
    } catch (const std::exception& e) {
        nlohmann::json errResp;
        errResp["success"] = false;
        errResp["error"] = e.what();
        std::string errStr = errResp.dump();
        if (sender) {
            sender->PostWebMessageAsJson(Utf8ToWide(errStr).c_str());
        }
    }
}

nlohmann::json NativeBridge::ProcessAction(const std::string& action, const nlohmann::json& params) {
    nlohmann::json res;
    res["success"] = true;

    if (action == "getInitialState") {
        return HandleGetInitialState();
    } else if (action == "getSystemSpecs") {
        return HandleGetSystemSpecs();
    } else if (action == "getTweaksState") {
        return HandleGetTweaksState();
    } else if (action == "getSystemSuiteState") {
        return HandleGetSystemSuiteState();
    } else if (action == "getServices") {
        return HandleGetServices();
    } else if (action == "getAppx") {
        return HandleGetAppx();
    } else if (action == "getCleanupOptions") {
        return HandleGetCleanupOptions();
    } else if (action == "getMemoryStats") {
        return HandleGetMemoryStats();
    }
    // Presets and core tweaks
    else if (action == "applyPreset") {
        return HandleApplyPreset(params);
    } else if (action == "applyAllTweaks") {
        return HandleApplyAllTweaks();
    } else if (action == "revertToDefaults") {
        return HandleRevertToDefaults();
    } else if (action == "setTweak" || action == "toggleTweak") {
        return HandleSetTweak(params);
    } else if (action == "setTimerLock") {
        return HandleSetTimerLock(params);
    } else if (action == "flushDNS") {
        return HandleFlushDNS();
    } else if (action == "purgeStandbyMemory") {
        return HandlePurgeStandbyMemory();
    }
    // System Suite
    else if (action == "applySystemTweaks") {
        return HandleApplySystemTweaks();
    } else if (action == "applySecurityTweaks") {
        return HandleApplySecurityTweaks();
    } else if (action == "applySingleSystemTweak") {
        return HandleApplySingleSystemTweak(params);
    } else if (action == "applySingleSecurityTweak") {
        return HandleApplySingleSecurityTweak(params);
    } else if (action == "applyOsPerfSubTweak") {
        return HandleApplyOsPerfSubTweak(params);
    } else if (action == "applyAllOsPerfSubTweaks") {
        return HandleApplyAllOsPerfSubTweaks(params);
    } else if (action == "applySsdPerfSubTweak") {
        return HandleApplySsdPerfSubTweak(params);
    } else if (action == "applyAllSsdPerfSubTweaks") {
        return HandleApplyAllSsdPerfSubTweaks(params);
    } else if (action == "applyTelemetrySubTweak") {
        return HandleApplyTelemetrySubTweak(params);
    } else if (action == "applyAllTelemetrySubTweaks") {
        return HandleApplyAllTelemetrySubTweaks(params);
    }
    // Context Menu
    else if (action == "getContextMenuTweaks") {
        return HandleGetContextMenuTweaks();
    } else if (action == "toggleContextMenuTweak") {
        return HandleToggleContextMenuTweak(params);
    } else if (action == "setContextMenuShiftOnly") {
        return HandleSetContextMenuShiftOnly(params);
    } else if (action == "applyAllContextMenuTweaks") {
        return HandleApplyAllContextMenuTweaks(params);
    } else if (action == "applyContextMenuGroup") {
        return HandleApplyContextMenuGroup(params);
    }
    // Windows 11 Tweaks
    else if (action == "getWin11Tweaks") {
        return HandleGetWin11Tweaks();
    } else if (action == "toggleWin11Tweak") {
        return HandleToggleWin11Tweak(params);
    }
    // Services
    else if (action == "setServiceState") {
        return HandleSetServiceState(params);
    } else if (action == "enableAllServices") {
        return HandleEnableAllServices();
    } else if (action == "disableRecommendedServices") {
        return HandleDisableRecommendedServices();
    }
    // Appx
    else if (action == "getAppx") {
        return HandleGetAppx();
    } else if (action == "scanAppx") {
        return HandleScanAppx();
    } else if (action == "selectAppx") {
        return HandleSelectAppx(params);
    } else if (action == "selectAllAppx") {
        return HandleSelectAllAppx(params);
    } else if (action == "uninstallSelectedAppx") {
        return HandleUninstallSelectedAppx();
    } else if (action == "uninstallAllAppx") {
        return HandleUninstallAllAppx();
    } else if (action == "restoreAppx") {
        return HandleRestoreAppx(params);
    } else if (action == "setAppxSetting") {
        return HandleSetAppxSetting(params);
    }
    // Cleanup
    else if (action == "setCleanupOption") {
        return HandleSetCleanupOption(params);
    } else if (action == "setAllCleanupOptions") {
        return HandleSetAllCleanupOptions(params);
    } else if (action == "executeCleanup") {
        return HandleExecuteCleanup(params);
    } else if (action == "deepCleanAction") {
        return HandleDeepCleanAction(params);
    }
    // Memory Cleaner
    else if (action == "trimWorkingSet") {
        return HandleTrimWorkingSet();
    } else if (action == "clearSystemCache") {
        return HandleClearSystemCache();
    } else if (action == "cleanAllMemory") {
        return HandleCleanAllMemory();
    } else if (action == "setMemConfig") {
        return HandleSetMemConfig(params);
    }
    // Backup & Restore
    else if (action == "createRestorePoint") {
        return HandleCreateRestorePoint(params);
    } else if (action == "exportProfile") {
        return HandleExportProfile();
    } else if (action == "importProfile") {
        return HandleImportProfile();
    }
    // Tools
    else if (action == "openSystemTool") {
        return HandleOpenSystemTool(params);
    } else if (action == "openMemCleanerWindow") {
        if (m_openMemCleanerCb) m_openMemCleanerCb();
        res["success"] = true;
        return res;
    }
    // Program Settings
    else if (action == "getConfig") {
        return HandleGetConfig();
    } else if (action == "getAutostart") {
        res["success"] = true;
        res["autostart"] = AutostartManager::Get().IsAutostartEnabled();
        return res;
    } else if (action == "saveConfig") {
        return HandleSaveConfig(params);
    } else if (action == "updateSetting") {
        return HandleUpdateSetting(params);
    }
    // Startup Manager
    else if (action == "getStartupItems" || action == "scanStartup") {
        return HandleScanStartup();
    } else if (action == "toggleStartupItem") {
        return HandleToggleStartupItem(params);
    } else if (action == "deleteStartupItem") {
        return HandleDeleteStartupItem(params);
    }
    // MSI Interrupt Manager
    else if (action == "getMsiDevices" || action == "scanDevices" || action == "scanMsiDevices") {
        return HandleScanDevices();
    } else if (action == "setMsiEnabled") {
        return HandleSetMsiEnabled(params);
    } else if (action == "setMsiPriority") {
        return HandleSetMsiPriority(params);
    } else if (action == "applyMsiEsportsProfile") {
        return HandleApplyMsiEsportsProfile();
    }
    // Game Boost Manager
    else if (action == "getGameBoostStatus") {
        return HandleGetGameBoostStatus();
    } else if (action == "updateGameBoostSettings") {
        return HandleUpdateGameBoostSettings(params);
    } else if (action == "addCustomBoostGame") {
        return HandleAddCustomBoostGame(params);
    } else if (action == "removeCustomBoostGame") {
        return HandleRemoveCustomBoostGame(params);
    } else if (action == "removeBoostGame" || action == "deleteBoostGame") {
        return HandleRemoveBoostGame(params);
    } else if (action == "fetchGameCover") {
        return HandleFetchGameCover(params);
    } else if (action == "scanInstalledGames") {
        res["success"] = true;
        nlohmann::json gamesList = GameBoostManager::Get().ScanInstalledGames();
        res["games"] = gamesList;
        res["installedGames"] = gamesList;
        return res;
    }
    else if (action == "checkUpdates") {
        return HandleCheckUpdates(params);
    } else if (action == "getUpdateInfo") {
        return HandleGetUpdateInfo();
    } else if (action == "openUpdateUrl") {
        return HandleOpenUpdateUrl();
    } else if (action == "openReleasePage") {
        UpdateManager::Get().OpenReleasePage();
        res["success"] = true;
        return res;
    }

    res["success"] = false;
    res["error"] = "Unknown action: " + action;
    return res;
}

// -----------------------------------------------------------------------------
// Action Handlers
// -----------------------------------------------------------------------------

nlohmann::json NativeBridge::HandleGetInitialState() {
    nlohmann::json root;
    root["specs"] = HandleGetSystemSpecs();
    root["tweaks"] = HandleGetTweaksState();
    root["systemSuite"] = HandleGetSystemSuiteState();
    root["contextMenu"] = ContextMenuManager::Get().GetStateJson();
    root["win11Tweaks"] = Win11Tweaker::Get().GetStateJson();
    root["services"] = HandleGetServices()["services"];
    root["appx"] = HandleGetAppx()["items"];
    root["cleanup"] = HandleGetCleanupOptions()["options"];
    root["memory"] = HandleGetMemoryStats();
    root["timerLockActive"] = TimerResolutionService::Get().IsActive();
    root["startup"] = StartupManager::Get().GetItemsJson();
    root["msi"] = MsiManager::Get().GetDevicesJson();
    root["gameBoost"] = GameBoostManager::Get().GetStatusJson();
    root["currentVersion"] = UpdateManager::Get().GetCurrentAppVersion();
    root["update"] = HandleGetUpdateInfo();

    auto& cfg = ConfigManager::Get().GetConfig();
    nlohmann::json config;
    config["runInBackground"] = cfg.runInBackground;
    config["startMinimizedToTray"] = cfg.startMinimizedToTray;
    config["autostartWithWindows"] = cfg.autostartWithWindows;
    config["autoRamCleanEnabled"] = cfg.autoRamCleanEnabled;
    config["ramThresholdPercent"] = cfg.ramThresholdPercent;
    config["ramCleanIntervalMinutes"] = cfg.ramCleanIntervalMinutes;
    config["trimWorkingSetOnThreshold"] = cfg.trimWorkingSetOnThreshold;
    config["clearCacheOnThreshold"] = cfg.clearCacheOnThreshold;
    config["trimWorkingSetOnInterval"] = cfg.trimWorkingSetOnInterval;
    config["clearCacheOnInterval"] = cfg.clearCacheOnInterval;
    config["themeAccent"] = cfg.themeAccent;
    config["gameBoostEnabled"] = cfg.gameBoostEnabled;
    config["boostHighPriority"] = cfg.boostHighPriority;
    config["boostStandbyPurge"] = cfg.boostStandbyPurge;
    config["boostLockTimer"] = cfg.boostLockTimer;
    config["customBoostGames"] = cfg.customBoostGames;
    config["autoCheckUpdates"] = cfg.autoCheckUpdates;
    config["updateChannel"] = cfg.updateChannel;
    root["config"] = config;

    root["success"] = true;
    return root;
}

nlohmann::json NativeBridge::HandleGetSystemSpecs() {
    auto specs = SystemInfo::Get().GetSpecs();
    nlohmann::json j;
    j["cpuName"] = specs.cpuName;
    j["logicalCores"] = specs.logicalCores;
    j["physicalCores"] = specs.physicalCores;
    j["totalRAM_GB"] = specs.totalRAM_GB;
    j["freeRAM_GB"] = specs.freeRAM_GB;
    j["osName"] = specs.osName;
    j["osBuild"] = specs.osBuild;
    j["activePowerPlan"] = specs.activePowerPlan;
    j["isUltimatePerformanceActive"] = specs.isUltimatePerformanceActive;
    j["primaryGPUName"] = specs.primaryGPUName;
    j["primaryGPUMSIEnabled"] = specs.primaryGPUMSIEnabled;
    j["currentTimerResolutionMs"] = specs.currentTimerResolutionMs;

    nlohmann::json gpus = nlohmann::json::array();
    for (const auto& g : specs.gpus) {
        nlohmann::json gj;
        gj["name"] = WideToUtf8(g.name);
        gj["pciDevicePath"] = WideToUtf8(g.pciDevicePath);
        gj["msiSupported"] = g.msiSupported;
        gj["msiEnabled"] = g.msiEnabled;
        gj["msiPriority"] = g.msiPriority;
        gj["dedicatedVRAM"] = g.dedicatedVRAM;
        gpus.push_back(gj);
    }
    j["gpus"] = gpus;
    j["success"] = true;
    return j;
}

nlohmann::json NativeBridge::HandleGetTweaksState() {
    TweaksEngine::Get().DetectFastState();
    auto state = TweaksEngine::Get().GetState();
    nlohmann::json t;
    t["timerResolution05ms"] = state.timerResolution05ms;
    t["disableDynamicTick"] = state.disableDynamicTick;
    t["usePlatformClockNo"] = state.usePlatformClockNo;
    t["usePlatformTickNo"] = state.usePlatformTickNo;
    t["usePlatformTick"] = state.usePlatformTick;
    t["bootuxDisabled"] = state.bootuxDisabled;
    t["bootux"] = state.bootuxDisabled;
    t["win32PrioritySeparation"] = state.win32PrioritySeparation;
    t["win32PriorityValue"] = state.win32PriorityValue;
    t["ultimatePowerPlan"] = state.ultimatePowerPlan;
    t["disableCoreParking"] = state.disableCoreParking;
    t["coreParking"] = state.disableCoreParking;
    t["powerThrottling"] = state.powerThrottling;

    t["gpuMsiMode"] = state.gpuMsiMode;
    t["audioMsiMode"] = state.audioMsiMode;
    t["hagsEnabled"] = state.hagsEnabled;
    t["hagsGpu"] = state.hagsEnabled;
    t["forceP0"] = state.forceP0;
    t["resizableBarDetected"] = state.resizableBarDetected;
    t["csrssDwmPriority"] = state.csrssDwmPriority;
    t["disableGameDVR"] = state.disableGameDVR;
    t["gameDVR"] = state.disableGameDVR;
    t["disableFullscreenOptimizations"] = state.disableFullscreenOptimizations;
    t["fsoOptimizations"] = state.disableFullscreenOptimizations;

    t["mmcssGamesPriority"] = state.mmcssGamesPriority;
    t["disableNetworkThrottling"] = state.disableNetworkThrottling;
    t["networkThrottling"] = state.disableNetworkThrottling;
    t["systemResponsiveness"] = state.mmcssGamesPriority;

    t["tcpNoDelay"] = state.tcpNoDelay;
    t["tcpAckFrequency"] = state.tcpAckFrequency;
    t["tcpAutoTuning"] = state.tcpAutoTuning;
    t["defaultTTL64"] = state.defaultTTL64;
    t["defaultTtl"] = state.defaultTTL64;
    t["optimalMTU"] = state.optimalMTU;

    t["disablePagingExecutive"] = state.disablePagingExecutive;
    t["largeSystemCacheDisabled"] = state.largeSystemCacheDisabled;
    t["largeSystemCache"] = state.largeSystemCacheDisabled;
    t["ioPageLockLimit"] = state.ioPageLockLimit;

    t["flatMouseCurve"] = state.flatMouseCurve;
    t["keyboardMaxSpeed"] = state.keyboardMaxSpeed;
    t["disableTelemetry"] = state.disableTelemetry;
    t["disableErrorReporting"] = state.disableErrorReporting;

    t["activePreset"] = static_cast<int>(TweaksEngine::Get().GetActivePreset());
    t["isBcdReady"] = TweaksEngine::Get().IsBcdReady();
    t["success"] = true;
    return t;
}

nlohmann::json NativeBridge::HandleGetSystemSuiteState() {
    auto& tweaker = SystemTweaker::Get();
    const auto& sys = tweaker.GetSystemConfig();
    const auto& sec = tweaker.GetSecurityConfig();
    const auto& os = tweaker.GetOsPerfSubTweaks();
    const auto& ssd = tweaker.GetSsdPerfSubTweaks();
    const auto& telem = tweaker.GetTelemetrySubTweaks();

    nlohmann::json root;

    nlohmann::json sysJ;
    sysJ["maxOsPerf"] = sys.maxOsPerf;
    sysJ["maxSsdPerf"] = sys.maxSsdPerf;
    sysJ["disablePowerThrottling"] = sys.disablePowerThrottling;
    sysJ["disableHibernation"] = sys.disableHibernation;
    sysJ["enableF8Boot"] = sys.enableF8Boot;
    sysJ["classicPhotoViewer"] = sys.classicPhotoViewer;
    sysJ["disableStickyKeys"] = sys.disableStickyKeys;
    sysJ["restartShellOnCrash"] = sys.restartShellOnCrash;
    sysJ["svchostGrouping"] = sys.svchostGrouping;
    sysJ["disableRemoteAssistance"] = sys.disableRemoteAssistance;
    sysJ["allowNetFramework4"] = sys.allowNetFramework4;
    sysJ["autoEndTasks"] = sys.autoEndTasks;
    sysJ["reduceHungAppTimeout"] = sys.reduceHungAppTimeout;
    sysJ["autoKillHungApps"] = sys.autoKillHungApps;
    sysJ["disablePersistBrowsers"] = sys.disablePersistBrowsers;
    sysJ["disableFinishSetup"] = sys.disableFinishSetup;
    sysJ["verboseStatus"] = sys.verboseStatus;
    sysJ["hideUserAccountsOnLogon"] = sys.hideUserAccountsOnLogon;
    sysJ["requireCtrlAltDel"] = sys.requireCtrlAltDel;
    sysJ["enableMsiSafeMode"] = sys.enableMsiSafeMode;
    sysJ["disableAllEventLogs"] = sys.disableAllEventLogs;
    sysJ["disableBsodAutoRestart"] = sys.disableBsodAutoRestart;
    sysJ["powerShellAcceleration"] = sys.powerShellAcceleration;
    root["system"] = sysJ;

    nlohmann::json secJ;
    secJ["disableAllTelemetry"] = sec.disableAllTelemetry;
    secJ["disableHiddenMonitoring"] = sec.disableHiddenMonitoring;
    secJ["removeSpyServices"] = sec.removeSpyServices;
    secJ["blockTelemetryDomains"] = sec.blockTelemetryDomains;
    secJ["disableSecurityCenterNotifications"] = sec.disableSecurityCenterNotifications;
    secJ["disableExeWarnings"] = sec.disableExeWarnings;
    secJ["disableDownloadWarnings"] = sec.disableDownloadWarnings;
    secJ["disableBrowserWarnings"] = sec.disableBrowserWarnings;
    secJ["disableUAC"] = sec.disableUAC;
    secJ["disableUsbAutorun"] = sec.disableUsbAutorun;
    secJ["disableCpuMitigations"] = sec.disableCpuMitigations;
    secJ["disableVbsHvci"] = sec.disableVbsHvci;
    secJ["blockFacebookSync"] = sec.blockFacebookSync;
    secJ["blockOutlookSync"] = sec.blockOutlookSync;
    secJ["blockSkypeSync"] = sec.blockSkypeSync;
    secJ["preventEdgeAutorun"] = sec.preventEdgeAutorun;
    secJ["hideRealTTL"] = sec.hideRealTTL;
    root["security"] = secJ;

    nlohmann::json osJ;
    osJ["waitScreenLock"] = os.waitScreenLock;
    osJ["noMonitorOff"] = os.noMonitorOff;
    osJ["noDiskSpindown"] = os.noDiskSpindown;
    osJ["ultimatePowerPlan"] = os.ultimatePowerPlan;
    osJ["alwaysUnloadDll"] = os.alwaysUnloadDll;
    osJ["nonPagedPoolFix"] = os.nonPagedPoolFix;
    osJ["disableBackgroundApps"] = os.disableBackgroundApps;
    osJ["disableAutoWindowsUpdate"] = os.disableAutoWindowsUpdate;
    osJ["disableDriverUpdates"] = os.disableDriverUpdates;
    osJ["disableReservedStorage"] = os.disableReservedStorage;
    osJ["disableTurboBoostAC"] = os.disableTurboBoostAC;
    osJ["disableTurboBoostDC"] = os.disableTurboBoostDC;
    osJ["disablePowerThrottling"] = os.disablePowerThrottling;
    osJ["disableHibernation"] = os.disableHibernation;
    root["osPerfSub"] = osJ;

    nlohmann::json ssdJ;
    ssdJ["disablePrefetchSuperfetch"] = ssd.disablePrefetchSuperfetch;
    ssdJ["disableBootDefrag"] = ssd.disableBootDefrag;
    ssdJ["disableLastAccess"] = ssd.disableLastAccess;
    ssdJ["disable8dot3Names"] = ssd.disable8dot3Names;
    ssdJ["enableTrimSupport"] = ssd.enableTrimSupport;
    ssdJ["disableLowDiskSpaceChecks"] = ssd.disableLowDiskSpaceChecks;
    ssdJ["disablePagingExecutive"] = ssd.disablePagingExecutive;
    ssdJ["largeSystemCache"] = ssd.largeSystemCache;
    ssdJ["win32PrioritySeparation"] = ssd.win32PrioritySeparation;
    root["ssdPerfSub"] = ssdJ;

    nlohmann::json telemJ;
    telemJ["disableAdvertisingId"] = telem.disableAdvertisingId;
    telemJ["disableSettingSync"] = telem.disableSettingSync;
    telemJ["disableVsTelemetry"] = telem.disableVsTelemetry;
    telemJ["disableOfficeTelemetry"] = telem.disableOfficeTelemetry;
    telemJ["disableNvidiaTelemetry"] = telem.disableNvidiaTelemetry;
    telemJ["disableCeipTasks"] = telem.disableCeipTasks;
    telemJ["disableAppInventory"] = telem.disableAppInventory;
    telemJ["disableAppUsageStats"] = telem.disableAppUsageStats;
    telemJ["disableInkingTypingData"] = telem.disableInkingTypingData;
    telemJ["disableUserBehavior"] = telem.disableUserBehavior;
    telemJ["disableLocation"] = telem.disableLocation;
    telemJ["disableFeedbackFrequency"] = telem.disableFeedbackFrequency;
    telemJ["disableSpeechModelUpdate"] = telem.disableSpeechModelUpdate;
    telemJ["disableExperiments"] = telem.disableExperiments;
    telemJ["disableAutoLoggerDiagTrack"] = telem.disableAutoLoggerDiagTrack;
    root["telemetrySub"] = telemJ;

    root["disableStoreAutoUpdate"] = tweaker.disableStoreAutoUpdate;
    root["disableStartRecommended"] = tweaker.disableStartRecommended;
    root["contextMenu"] = ContextMenuManager::Get().GetStateJson();
    root["success"] = true;
    return root;
}

nlohmann::json NativeBridge::HandleGetServices() {
    auto& tweaker = SystemTweaker::Get();
    const auto& svcs = tweaker.GetServices();
    nlohmann::json list = nlohmann::json::array();
    for (const auto& s : svcs) {
        nlohmann::json item;
        item["id"] = s.id;
        item["displayName"] = s.displayName;
        item["description"] = s.description;
        item["recommendedDisable"] = s.recommendedDisable;
        item["isDanger"] = s.isDanger;
        item["disabled"] = s.disabled;
        item["status"] = s.status.empty() ? s.statusText : s.status;
        item["isRunning"] = s.isRunning || (s.currentServiceState == SERVICE_RUNNING);
        list.push_back(item);
    }
    nlohmann::json res;
    res["services"] = list;
    res["disabledCount"] = tweaker.GetDisabledServicesCount();
    res["totalCount"] = (int)svcs.size();
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleGetAppx() {
    auto& tweaker = SystemTweaker::Get();
    const auto& items = tweaker.GetAppxItems();
    nlohmann::json list = nlohmann::json::array();
    for (const auto& a : items) {
        nlohmann::json item;
        item["id"] = a.id;
        item["displayName"] = a.displayName;
        item["category"] = a.category;
        item["selected"] = a.selected;
        item["isInstalled"] = a.isInstalled;
        list.push_back(item);
    }
    nlohmann::json res;
    res["items"] = list;
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleGetCleanupOptions() {
    const auto& c = SystemTweaker::Get().GetCleanupOptions();
    nlohmann::json opts;
    opts["cleanDefenderCache"] = c.cleanDefenderCache;
    opts["cleanUsbHistory"] = c.cleanUsbHistory;
    opts["cleanWindowsInstallCache"] = c.cleanWindowsInstallCache;
    opts["cleanEventLogs"] = c.cleanEventLogs;
    opts["cleanSoftwareDistribution"] = c.cleanSoftwareDistribution;
    opts["cleanTempFiles"] = c.cleanTempFiles;
    opts["cleanExplorerThumbnails"] = c.cleanExplorerThumbnails;
    opts["cleanDiagnosticLogs"] = c.cleanDiagnosticLogs;
    opts["cleanBrowserCache"] = c.cleanBrowserCache;
    opts["cleanSystemReports"] = c.cleanSystemReports;
    opts["cleanOfficeCache"] = c.cleanOfficeCache;
    opts["cleanGpuCache"] = c.cleanGpuCache;

    nlohmann::json res;
    res["options"] = opts;
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleGetMemoryStats() {
    auto mem = MemoryCleaner::Get().QueryDetailedStats(false);
    nlohmann::json j;
    j["totalRAM_GB"] = mem.totalRAM_GB;
    j["usedRAM_GB"] = mem.usedRAM_GB;
    j["freeRAM_GB"] = mem.freeRAM_GB;
    j["standbyRAM_GB"] = mem.standbyRAM_GB;
    j["usedPercent"] = mem.usedPercent;
    j["pagefileTotal_GB"] = mem.pagefileTotal_GB;
    j["pagefileUsed_GB"] = mem.pagefileUsed_GB;
    j["pagefilePercent"] = mem.pagefilePercent;
    j["peakMemoryPercent"] = mem.peakMemoryPercent;
    j["remainingSeconds"] = MemoryCleaner::Get().GetRemainingSeconds();
    j["timerProgress"] = MemoryCleaner::Get().GetTimerProgress();
    j["lastFreedBytes"] = MemoryCleaner::Get().GetLastFreedBytes();
    j["totalSessionFreedBytes"] = MemoryCleaner::Get().GetTotalSessionFreedBytes();
    j["lastActionStatus"] = MemoryCleaner::Get().GetLastActionStatus();
    j["success"] = true;
    return j;
}

nlohmann::json NativeBridge::HandleApplyPreset(const nlohmann::json& params) {
    std::string presetStr = params.value("preset", "EsportsUltra");
    std::string status;
    bool ok = TweaksEngine::Get().ApplyPreset(presetStr, status);

    // Save active preset into config
    auto& cfg = ConfigManager::Get().GetConfig();
    cfg.tweaks = TweaksEngine::Get().GetState();
    cfg.activePreset = static_cast<int>(TweaksEngine::Get().GetActivePreset());
    ConfigManager::Get().Save();

    ShowToast(status, !ok);

    nlohmann::json res;
    res["success"] = ok;
    res["status"] = status;
    res["tweaks"] = HandleGetTweaksState();
    return res;
}

nlohmann::json NativeBridge::HandleApplyAllTweaks() {
    std::string status;
    bool ok = TweaksEngine::Get().ApplyAll(status);

    auto& cfg = ConfigManager::Get().GetConfig();
    cfg.tweaks = TweaksEngine::Get().GetState();
    cfg.activePreset = static_cast<int>(TweaksEngine::Get().GetActivePreset());
    ConfigManager::Get().Save();

    ShowToast(status, !ok);

    nlohmann::json res;
    res["success"] = ok;
    res["status"] = status;
    res["tweaks"] = HandleGetTweaksState();
    return res;
}

nlohmann::json NativeBridge::HandleRevertToDefaults() {
    std::string status;
    bool ok = TweaksEngine::Get().RevertToDefaults(status);

    auto& cfg = ConfigManager::Get().GetConfig();
    cfg.tweaks = TweaksEngine::Get().GetState();
    cfg.activePreset = static_cast<int>(PresetType::WindowsDefault);
    ConfigManager::Get().Save();

    ShowToast(status, !ok);

    nlohmann::json res;
    res["success"] = ok;
    res["status"] = status;
    res["tweaks"] = HandleGetTweaksState();
    return res;
}

nlohmann::json NativeBridge::HandleSetTweak(const nlohmann::json& params) {
    std::string tweakId = params.value("id", "");
    bool val = params.value("value", params.value("enabled", false));
    if (ContextMenuManager::Get().HasTweak(tweakId)) {
        std::string msg;
        bool ok = ContextMenuManager::Get().ToggleTweak(tweakId, val, msg);
        ShowToast(msg, !ok);
        nlohmann::json res;
        res["success"] = ok;
        res["status"] = msg;
        res["items"] = ContextMenuManager::Get().GetStateJson();
        return res;
    }

    if (Win11Tweaker::Get().HasTweak(tweakId)) {
        std::string msg;
        bool ok = Win11Tweaker::Get().ToggleTweak(tweakId, val, msg);
        ShowToast(msg, !ok);
        nlohmann::json res = Win11Tweaker::Get().GetStateJson();
        res["success"] = ok;
        res["status"] = msg;
        return res;
    }

    bool ok = false;
    auto& engine = TweaksEngine::Get();

    if (tweakId == "timerResolution05ms") ok = engine.SetTimerResolution(val);
    else if (tweakId == "disableDynamicTick") ok = engine.SetDynamicTick(val);
    else if (tweakId == "usePlatformClockNo") ok = engine.SetPlatformClock(val);
    else if (tweakId == "usePlatformTickNo" || tweakId == "usePlatformTick") ok = engine.SetPlatformTick(val);
    else if (tweakId == "bootuxDisabled" || tweakId == "bootux") ok = engine.SetBootUX(val);
    else if (tweakId == "win32PrioritySeparation") ok = engine.SetWin32PrioritySeparation(val);
    else if (tweakId == "ultimatePowerPlan") ok = engine.SetUltimatePowerPlan(val);
    else if (tweakId == "disableCoreParking" || tweakId == "coreParking") ok = engine.SetCoreParking(val);
    else if (tweakId == "powerThrottling") ok = engine.SetPowerThrottling(val);
    else if (tweakId == "gpuMsiMode") ok = engine.SetGPUMsiMode(val);
    else if (tweakId == "audioMsiMode") ok = engine.SetAudioMsiMode(val);
    else if (tweakId == "hagsEnabled" || tweakId == "hagsGpu") ok = engine.SetHAGS(val);
    else if (tweakId == "forceP0") ok = engine.SetForceP0(val);
    else if (tweakId == "csrssDwmPriority") ok = engine.SetCsrssDwmPriority(val);
    else if (tweakId == "disableGameDVR" || tweakId == "gameDVR") ok = engine.SetGameDVR(val);
    else if (tweakId == "disableFullscreenOptimizations" || tweakId == "fsoOptimizations") ok = engine.SetFullscreenOptimizations(val);
    else if (tweakId == "mmcssGamesPriority" || tweakId == "systemResponsiveness") ok = engine.SetMMCSSGamesPriority(val);
    else if (tweakId == "disableNetworkThrottling" || tweakId == "networkThrottling") ok = engine.SetNetworkThrottling(val);
    else if (tweakId == "tcpNoDelay") ok = engine.SetTCPNoDelay(val);
    else if (tweakId == "tcpAckFrequency") ok = engine.SetTCPAckFrequency(val);
    else if (tweakId == "tcpAutoTuning") ok = engine.SetTCPAutoTuning(val);
    else if (tweakId == "defaultTTL64" || tweakId == "defaultTtl") ok = engine.SetDefaultTTL(val);
    else if (tweakId == "optimalMTU") ok = engine.SetNetworkMTU(val);
    else if (tweakId == "disablePagingExecutive") ok = engine.SetDisablePagingExecutive(val);
    else if (tweakId == "largeSystemCacheDisabled" || tweakId == "largeSystemCache") ok = engine.SetLargeSystemCache(val);
    else if (tweakId == "ioPageLockLimit") ok = engine.SetIoPageLockLimit(val);
    else if (tweakId == "flatMouseCurve") ok = engine.SetFlatMouseCurve(val);
    else if (tweakId == "keyboardMaxSpeed") ok = engine.SetKeyboardSpeed(val);
    else if (tweakId == "disableTelemetry") ok = engine.SetTelemetry(val);
    else if (tweakId == "disableErrorReporting") ok = engine.SetErrorReporting(val);

    auto& cfg = ConfigManager::Get().GetConfig();
    cfg.tweaks = engine.GetState();
    cfg.activePreset = static_cast<int>(engine.GetActivePreset());
    ConfigManager::Get().Save();

    nlohmann::json res;
    res["success"] = ok;
    res["id"] = tweakId;
    res["value"] = val;
    res["tweaks"] = HandleGetTweaksState();
    return res;
}

nlohmann::json NativeBridge::HandleSetTimerLock(const nlohmann::json& params) {
    bool enable = params.value("enable", true);
    if (enable) {
        TimerResolutionService::Get().Start(0.500f);
        TweaksEngine::Get().SetTimerResolution(true);
        ShowToast("Таймер 0.500 ms успешно заблокирован", false);
    } else {
        TimerResolutionService::Get().Stop();
        TweaksEngine::Get().SetTimerResolution(false);
        ShowToast("Блокировка таймера снята", false);
    }
    BroadcastTelemetry();
    nlohmann::json res;
    res["success"] = true;
    res["timerLockActive"] = TimerResolutionService::Get().IsActive();
    return res;
}

nlohmann::json NativeBridge::HandleFlushDNS() {
    bool ok = TweaksEngine::Get().FlushDNS();
    if (ok) ShowToast("DNS-кэш успешно очищен", false);
    else ShowToast("Ошибка при сбросе DNS-кэша", true);
    nlohmann::json res;
    res["success"] = ok;
    return res;
}

nlohmann::json NativeBridge::HandlePurgeStandbyMemory() {
    std::string msg;
    bool ok = TweaksEngine::Get().PurgeStandbyMemory(msg);
    ShowToast(msg, !ok);
    BroadcastTelemetry();
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplySystemTweaks() {
    std::string msg;
    bool ok = SystemTweaker::Get().ApplySystemTweaks(msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplySecurityTweaks() {
    std::string msg;
    bool ok = SystemTweaker::Get().ApplySecurityTweaks(msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplySingleSystemTweak(const nlohmann::json& params) {
    std::string tweakId = params.value("id", "");
    bool val = params.value("value", false);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplySingleSystemTweak(tweakId, val, msg);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplySingleSecurityTweak(const nlohmann::json& params) {
    std::string tweakId = params.value("id", "");
    bool val = params.value("value", false);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplySingleSecurityTweak(tweakId, val, msg);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplyOsPerfSubTweak(const nlohmann::json& params) {
    std::string tweakId = params.value("id", "");
    bool val = params.value("value", false);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplyOsPerfSubTweak(tweakId, val, msg);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplyAllOsPerfSubTweaks(const nlohmann::json& params) {
    bool enable = params.value("enable", true);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplyAllOsPerfSubTweaks(enable, msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplySsdPerfSubTweak(const nlohmann::json& params) {
    std::string tweakId = params.value("id", "");
    bool val = params.value("value", false);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplySsdPerfSubTweak(tweakId, val, msg);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplyAllSsdPerfSubTweaks(const nlohmann::json& params) {
    bool enable = params.value("enable", true);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplyAllSsdPerfSubTweaks(enable, msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplyTelemetrySubTweak(const nlohmann::json& params) {
    std::string tweakId = params.value("id", "");
    bool val = params.value("value", false);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplyTelemetrySubTweak(tweakId, val, msg);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplyAllTelemetrySubTweaks(const nlohmann::json& params) {
    bool enable = params.value("enable", true);
    std::string msg;
    bool ok = SystemTweaker::Get().ApplyAllTelemetrySubTweaks(enable, msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleSetServiceState(const nlohmann::json& params) {
    std::string svcId = params.value("id", "");
    bool disable = params.value("disable", false);

    auto& svcs = SystemTweaker::Get().GetServices();
    bool found = false;
    ServiceItemInfo* targetSvc = nullptr;
    for (auto& s : svcs) {
        if (s.id == svcId) {
            SystemTweaker::Get().ApplyServiceState(s, disable);
            targetSvc = &s;
            found = true;
            break;
        }
    }
    nlohmann::json res;
    res["success"] = found;
    res["id"] = svcId;
    res["disabled"] = disable;
    if (found && targetSvc) {
        res["service"] = {
            {"id", targetSvc->id},
            {"displayName", targetSvc->displayName},
            {"description", targetSvc->description},
            {"recommendedDisable", targetSvc->recommendedDisable},
            {"isDanger", targetSvc->isDanger},
            {"disabled", targetSvc->disabled},
            {"isRunning", targetSvc->isRunning || (targetSvc->currentServiceState == SERVICE_RUNNING)},
            {"status", targetSvc->status.empty() ? targetSvc->statusText : targetSvc->status}
        };
    }
    return res;
}

nlohmann::json NativeBridge::HandleEnableAllServices() {
    std::string msg;
    bool ok = SystemTweaker::Get().EnableAllServices(msg);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    res["message"] = msg;
    res["services"] = HandleGetServices()["services"];
    return res;
}

nlohmann::json NativeBridge::HandleDisableRecommendedServices() {
    std::string msg;
    bool ok = SystemTweaker::Get().DisableRecommendedServices(msg);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    res["message"] = msg;
    res["services"] = HandleGetServices()["services"];
    return res;
}

nlohmann::json NativeBridge::HandleSelectAppx(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    bool sel = params.value("selected", false);
    for (auto& a : SystemTweaker::Get().GetAppxItems()) {
        if (a.id == id) {
            a.selected = sel;
            break;
        }
    }
    nlohmann::json res;
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleSelectAllAppx(const nlohmann::json& params) {
    bool sel = params.value("select", true);
    SystemTweaker::Get().SelectAllAppx(sel);
    nlohmann::json res;
    res["success"] = true;
    res["appx"] = HandleGetAppx()["items"];
    return res;
}

nlohmann::json NativeBridge::HandleUninstallSelectedAppx() {
    std::thread([this]() {
        std::string msg;
        bool ok = SystemTweaker::Get().UninstallSelectedAppx([this](const std::string& p) {
            nlohmann::json j;
            j["progress"] = p;
            BroadcastEvent("appx_uninstall_progress", j);
        }, msg);
        ShowToast(msg, !ok);
        nlohmann::json doneEvt;
        doneEvt["success"] = ok;
        doneEvt["message"] = msg;
        doneEvt["appx"] = HandleGetAppx()["items"];
        BroadcastEvent("appx_uninstall_done", doneEvt);
    }).detach();

    nlohmann::json res;
    res["success"] = true;
    res["status"] = "Запущено удаление выбранных UWP приложений...";
    return res;
}

nlohmann::json NativeBridge::HandleUninstallAllAppx() {
    std::thread([this]() {
        std::string msg;
        bool ok = SystemTweaker::Get().UninstallAllAppx([this](const std::string& p) {
            nlohmann::json j;
            j["progress"] = p;
            BroadcastEvent("appx_uninstall_progress", j);
        }, msg);
        ShowToast(msg, !ok);
        nlohmann::json doneEvt;
        doneEvt["success"] = ok;
        doneEvt["message"] = msg;
        doneEvt["appx"] = HandleGetAppx()["items"];
        BroadcastEvent("appx_uninstall_done", doneEvt);
    }).detach();

    nlohmann::json res;
    res["success"] = true;
    res["status"] = "Запущено удаление всех доступных UWP приложений...";
    return res;
}

nlohmann::json NativeBridge::HandleRestoreAppx(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    std::string outMsg;
    bool ok = SystemTweaker::Get().RestoreAppxPackage(id, outMsg);

    nlohmann::json appJson = nlohmann::json::object();
    for (const auto& a : SystemTweaker::Get().GetAppxItems()) {
        if (a.id == id) {
            appJson["id"] = a.id;
            appJson["displayName"] = a.displayName;
            appJson["packageNamePattern"] = a.packageNamePattern;
            appJson["category"] = a.category;
            appJson["selected"] = a.selected;
            appJson["isInstalled"] = a.isInstalled;
            break;
        }
    }

    nlohmann::json res;
    res["success"] = ok;
    res["message"] = outMsg;
    res["app"] = appJson;
    return res;
}

nlohmann::json NativeBridge::HandleSetAppxSetting(const nlohmann::json& params) {
    std::string key = params.value("key", "");
    bool val = params.value("value", false);
    if (key == "disableStoreAutoUpdate") {
        SystemTweaker::Get().disableStoreAutoUpdate = val;
    } else if (key == "disableStartRecommended") {
        SystemTweaker::Get().disableStartRecommended = val;
    }
    std::string msg;
    SystemTweaker::Get().ApplyAppxSettings(msg);
    nlohmann::json res;
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleSetCleanupOption(const nlohmann::json& params) {
    std::string key = params.value("key", "");
    bool val = params.value("value", false);
    auto& c = SystemTweaker::Get().GetCleanupOptions();

    if (key == "cleanDefenderCache") c.cleanDefenderCache = val;
    else if (key == "cleanUsbHistory") c.cleanUsbHistory = val;
    else if (key == "cleanWindowsInstallCache") c.cleanWindowsInstallCache = val;
    else if (key == "cleanEventLogs") c.cleanEventLogs = val;
    else if (key == "cleanSoftwareDistribution") c.cleanSoftwareDistribution = val;
    else if (key == "cleanTempFiles") c.cleanTempFiles = val;
    else if (key == "cleanExplorerThumbnails") c.cleanExplorerThumbnails = val;
    else if (key == "cleanDiagnosticLogs") c.cleanDiagnosticLogs = val;
    else if (key == "cleanBrowserCache") c.cleanBrowserCache = val;
    else if (key == "cleanSystemReports") c.cleanSystemReports = val;
    else if (key == "cleanOfficeCache") c.cleanOfficeCache = val;
    else if (key == "cleanGpuCache") c.cleanGpuCache = val;

    nlohmann::json res;
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleSetAllCleanupOptions(const nlohmann::json& params) {
    bool sel = params.value("select", true);
    SystemTweaker::Get().SetAllCleanupOptions(sel);
    nlohmann::json res;
    res["success"] = true;
    res["cleanup"] = HandleGetCleanupOptions()["options"];
    return res;
}

nlohmann::json NativeBridge::HandleExecuteCleanup(const nlohmann::json& params) {
    if (params.contains("options") && params["options"].is_object()) {
        auto& c = SystemTweaker::Get().GetCleanupOptions();
        const auto& o = params["options"];
        if (o.contains("cleanDefenderCache")) c.cleanDefenderCache = o["cleanDefenderCache"].get<bool>();
        if (o.contains("cleanUsbHistory")) c.cleanUsbHistory = o["cleanUsbHistory"].get<bool>();
        if (o.contains("cleanWindowsInstallCache")) c.cleanWindowsInstallCache = o["cleanWindowsInstallCache"].get<bool>();
        if (o.contains("cleanEventLogs")) c.cleanEventLogs = o["cleanEventLogs"].get<bool>();
        if (o.contains("cleanSoftwareDistribution")) c.cleanSoftwareDistribution = o["cleanSoftwareDistribution"].get<bool>();
        if (o.contains("cleanTempFiles")) c.cleanTempFiles = o["cleanTempFiles"].get<bool>();
        if (o.contains("cleanExplorerThumbnails")) c.cleanExplorerThumbnails = o["cleanExplorerThumbnails"].get<bool>();
        if (o.contains("cleanDiagnosticLogs")) c.cleanDiagnosticLogs = o["cleanDiagnosticLogs"].get<bool>();
        if (o.contains("cleanBrowserCache")) c.cleanBrowserCache = o["cleanBrowserCache"].get<bool>();
        if (o.contains("cleanSystemReports")) c.cleanSystemReports = o["cleanSystemReports"].get<bool>();
        if (o.contains("cleanOfficeCache")) c.cleanOfficeCache = o["cleanOfficeCache"].get<bool>();
        if (o.contains("cleanGpuCache")) c.cleanGpuCache = o["cleanGpuCache"].get<bool>();
    }

    auto opts = SystemTweaker::Get().GetCleanupOptions();
    bool anyChecked = opts.cleanDefenderCache || opts.cleanUsbHistory || opts.cleanWindowsInstallCache ||
                      opts.cleanEventLogs || opts.cleanSoftwareDistribution || opts.cleanTempFiles ||
                      opts.cleanExplorerThumbnails || opts.cleanDiagnosticLogs || opts.cleanBrowserCache ||
                      opts.cleanSystemReports || opts.cleanOfficeCache || opts.cleanGpuCache;

    if (!anyChecked) {
        ShowToast("Выберите хотя бы один пункт для очистки!", true);
        nlohmann::json res;
        res["success"] = false;
        res["error"] = "Выберите хотя бы один пункт для очистки!";
        return res;
    }

    uint64_t freedBytes = 0;
    std::string msg;
    bool ok = SystemTweaker::Get().ExecuteCleanup(opts, freedBytes, msg);

    nlohmann::json res;
    res["success"] = ok;
    res["freedBytes"] = freedBytes;
    res["message"] = msg;

    ShowToast(msg, !ok);
    BroadcastEvent("cleanup_done", res);
    BroadcastTelemetry();
    return res;
}

nlohmann::json NativeBridge::HandleDeepCleanAction(const nlohmann::json& params) {
    std::string act = params.value("action", "");
    std::string msg;
    bool ok = false;
    auto& tw = SystemTweaker::Get();

    if (act == "terminate_processes") ok = tw.TerminateBackgroundProcesses(msg);
    else if (act == "clean_ram") ok = tw.CleanRamWorkingSets(msg);
    else if (act == "clean_junk") ok = tw.CleanJunkFolders(msg);
    else if (act == "clean_registry") ok = tw.CleanRegistryJunk(msg);
    else if (act == "clean_winsxs") ok = tw.CleanComponentStoreWinSxS(msg);
    else if (act == "clean_driverstore") ok = tw.CleanDriverStore(msg);
    else if (act == "revert_all_tweaks") ok = tw.RevertAllTweaks(msg);

    ShowToast(msg, !ok);
    BroadcastTelemetry();

    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleTrimWorkingSet() {
    size_t freed = 0;
    std::string status;
    bool ok = MemoryCleaner::Get().TrimProcessesWorkingSet(freed, status);
    ShowToast(status, !ok);
    BroadcastTelemetry();
    nlohmann::json res;
    res["success"] = ok;
    res["freedBytes"] = freed;
    res["status"] = status;
    return res;
}

nlohmann::json NativeBridge::HandleClearSystemCache() {
    size_t freed = 0;
    std::string status;
    bool ok = MemoryCleaner::Get().ClearSystemCache(freed, status);
    ShowToast(status, !ok);
    BroadcastTelemetry();
    nlohmann::json res;
    res["success"] = ok;
    res["freedBytes"] = freed;
    res["status"] = status;
    return res;
}

nlohmann::json NativeBridge::HandleCleanAllMemory() {
    size_t freed = 0;
    std::string status;
    bool ok = MemoryCleaner::Get().CleanAll(freed, status);
    ShowToast(status, !ok);
    BroadcastTelemetry();
    nlohmann::json res;
    res["success"] = ok;
    res["freedBytes"] = freed;
    res["status"] = status;
    return res;
}

nlohmann::json NativeBridge::HandleSetMemConfig(const nlohmann::json& params) {
    auto& cfg = ConfigManager::Get().GetConfig();
    if (params.contains("autoRamCleanEnabled")) cfg.autoRamCleanEnabled = params["autoRamCleanEnabled"].get<bool>();
    if (params.contains("ramThresholdPercent")) cfg.ramThresholdPercent = params["ramThresholdPercent"].get<int>();
    if (params.contains("ramCleanIntervalMinutes")) cfg.ramCleanIntervalMinutes = params["ramCleanIntervalMinutes"].get<int>();
    if (params.contains("trimWorkingSetOnThreshold")) cfg.trimWorkingSetOnThreshold = params["trimWorkingSetOnThreshold"].get<bool>();
    if (params.contains("clearCacheOnThreshold")) cfg.clearCacheOnThreshold = params["clearCacheOnThreshold"].get<bool>();
    if (params.contains("trimWorkingSetOnInterval")) cfg.trimWorkingSetOnInterval = params["trimWorkingSetOnInterval"].get<bool>();
    if (params.contains("clearCacheOnInterval")) cfg.clearCacheOnInterval = params["clearCacheOnInterval"].get<bool>();
    if (params.contains("runInBackground")) cfg.runInBackground = params["runInBackground"].get<bool>();
    if (params.contains("autostartWithWindows")) {
        cfg.autostartWithWindows = params["autostartWithWindows"].get<bool>();
        AutostartManager::Get().SetAutostart(cfg.autostartWithWindows);
    }

    ConfigManager::Get().Save();
    ShowToast("Настройки памяти успешно сохранены", false);
    nlohmann::json res;
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleCreateRestorePoint(const nlohmann::json& params) {
    std::string desc = params.value("description", "Windows Config Studio Optimization Restore Point");
    RestoreManager::Get().CreateSystemRestorePointAsync(Utf8ToWide(desc), [this](bool ok, const std::string& msg) {
        ShowToast(msg, !ok);
        nlohmann::json j;
        j["success"] = ok;
        j["message"] = msg;
        BroadcastEvent("restore_point_done", j);
    });
    nlohmann::json res;
    res["success"] = true;
    res["status"] = "Создание точки восстановления запущено...";
    return res;
}

nlohmann::json NativeBridge::HandleExportProfile() {
    wchar_t szFile[MAX_PATH] = L"wcs_profile.wcs";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.lpstrFilter = L"WCS Config Profile (*.wcs)\0*.wcs\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = L"wcs";

    nlohmann::json res;
    if (GetSaveFileNameW(&ofn)) {
        std::string msg;
        bool ok = RestoreManager::Get().ExportProfile(szFile, msg);
        ShowToast(msg, !ok);
        res["success"] = ok;
        res["status"] = msg;
    } else {
        res["success"] = false;
        res["status"] = "Отменено пользователем";
    }
    return res;
}

nlohmann::json NativeBridge::HandleImportProfile() {
    wchar_t szFile[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.lpstrFilter = L"WCS Config Profile (*.wcs)\0*.wcs\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    nlohmann::json res;
    if (GetOpenFileNameW(&ofn)) {
        std::string msg;
        bool ok = RestoreManager::Get().ImportProfile(szFile, msg);
        ShowToast(msg, !ok);
        res["success"] = ok;
        res["status"] = msg;
        res["tweaks"] = HandleGetTweaksState();
    } else {
        res["success"] = false;
        res["status"] = "Отменено пользователем";
    }
    return res;
}

nlohmann::json NativeBridge::HandleOpenSystemTool(const nlohmann::json& params) {
    std::string tool = params.value("tool", "");
    if (tool == "power") {
        ShellExecuteW(NULL, L"open", L"control.exe", L"/name Microsoft.PowerOptions", NULL, SW_SHOWNORMAL);
    } else if (tool == "devmgmt") {
        ShellExecuteW(NULL, L"open", L"devmgmt.msc", NULL, NULL, SW_SHOWNORMAL);
    } else if (tool == "ncpa") {
        ShellExecuteW(NULL, L"open", L"ncpa.cpl", NULL, NULL, SW_SHOWNORMAL);
    }
    nlohmann::json res;
    res["success"] = true;
    return res;
}

// -----------------------------------------------------------------------------
// Context Menu Handlers
// -----------------------------------------------------------------------------
nlohmann::json NativeBridge::HandleGetContextMenuTweaks() {
    nlohmann::json res;
    res["success"] = true;
    res["items"] = ContextMenuManager::Get().GetStateJson();
    return res;
}

nlohmann::json NativeBridge::HandleToggleContextMenuTweak(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    bool enabled = params.value("enabled", params.value("value", false));
    std::string msg;
    bool ok = ContextMenuManager::Get().ToggleTweak(id, enabled, msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    res["items"] = ContextMenuManager::Get().GetStateJson();
    return res;
}

nlohmann::json NativeBridge::HandleSetContextMenuShiftOnly(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    bool shiftOnly = params.value("shiftOnly", false);
    std::string msg;
    bool ok = ContextMenuManager::Get().SetShiftOnly(id, shiftOnly, msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    res["items"] = ContextMenuManager::Get().GetStateJson();
    return res;
}

nlohmann::json NativeBridge::HandleApplyAllContextMenuTweaks(const nlohmann::json& params) {
    bool enable = params.value("enable", true);
    std::string msg;
    bool ok = ContextMenuManager::Get().ApplyAll(enable, msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    res["items"] = ContextMenuManager::Get().GetStateJson();
    return res;
}

nlohmann::json NativeBridge::HandleApplyContextMenuGroup(const nlohmann::json& params) {
    std::string group = params.value("group", "");
    bool enable = params.value("enable", true);
    std::string msg;
    bool ok = ContextMenuManager::Get().ApplyGroup(group, enable, msg);
    ShowToast(msg, !ok);
    nlohmann::json res;
    res["success"] = ok;
    res["status"] = msg;
    res["items"] = ContextMenuManager::Get().GetStateJson();
    return res;
}

// -----------------------------------------------------------------------------
// Program Settings Handlers
// -----------------------------------------------------------------------------
nlohmann::json NativeBridge::HandleGetConfig() {
    auto& cfg = ConfigManager::Get().GetConfig();
    nlohmann::json res;
    res["success"] = true;
    res["config"] = {
        { "runInBackground", cfg.runInBackground },
        { "startMinimizedToTray", cfg.startMinimizedToTray },
        { "autostartWithWindows", cfg.autostartWithWindows },
        { "autoRamCleanEnabled", cfg.autoRamCleanEnabled },
        { "ramCleanIntervalMinutes", cfg.ramCleanIntervalMinutes },
        { "ramThresholdPercent", cfg.ramThresholdPercent },
        { "trimWorkingSetOnThreshold", cfg.trimWorkingSetOnThreshold },
        { "clearCacheOnThreshold", cfg.clearCacheOnThreshold },
        { "trimWorkingSetOnInterval", cfg.trimWorkingSetOnInterval },
        { "clearCacheOnInterval", cfg.clearCacheOnInterval },
        { "activePreset", cfg.activePreset },
        { "themeAccent", cfg.themeAccent },
        { "autoCheckUpdates", cfg.autoCheckUpdates },
        { "updateChannel", cfg.updateChannel }
    };
    return res;
}

nlohmann::json NativeBridge::HandleSaveConfig(const nlohmann::json& params) {
    auto& cfg = ConfigManager::Get().GetConfig();
    if (params.contains("config") && params["config"].is_object()) {
        const auto& c = params["config"];
        if (c.contains("runInBackground")) cfg.runInBackground = c["runInBackground"].get<bool>();
        if (c.contains("startMinimizedToTray")) cfg.startMinimizedToTray = c["startMinimizedToTray"].get<bool>();
        if (c.contains("autostartWithWindows")) {
            cfg.autostartWithWindows = c["autostartWithWindows"].get<bool>();
            AutostartManager::Get().SetAutostart(cfg.autostartWithWindows);
        }
        if (c.contains("autoRamCleanEnabled")) cfg.autoRamCleanEnabled = c["autoRamCleanEnabled"].get<bool>();
        if (c.contains("ramCleanIntervalMinutes")) cfg.ramCleanIntervalMinutes = c["ramCleanIntervalMinutes"].get<int>();
        if (c.contains("ramThresholdPercent")) cfg.ramThresholdPercent = c["ramThresholdPercent"].get<int>();
        if (c.contains("trimWorkingSetOnThreshold")) cfg.trimWorkingSetOnThreshold = c["trimWorkingSetOnThreshold"].get<bool>();
        if (c.contains("clearCacheOnThreshold")) cfg.clearCacheOnThreshold = c["clearCacheOnThreshold"].get<bool>();
        if (c.contains("trimWorkingSetOnInterval")) cfg.trimWorkingSetOnInterval = c["trimWorkingSetOnInterval"].get<bool>();
        if (c.contains("clearCacheOnInterval")) cfg.clearCacheOnInterval = c["clearCacheOnInterval"].get<bool>();
        if (c.contains("autoCheckUpdates")) cfg.autoCheckUpdates = c["autoCheckUpdates"].get<bool>();
        if (c.contains("updateChannel")) cfg.updateChannel = c["updateChannel"].get<std::string>();
        if (c.contains("themeAccent")) {
            cfg.themeAccent = c["themeAccent"].get<std::string>();
            BroadcastEvent("theme_changed", { {"theme", cfg.themeAccent} });
        }
    }
    ConfigManager::Get().Save();
    nlohmann::json res;
    res["success"] = true;
    res["config"] = HandleGetConfig()["config"];
    return res;
}

nlohmann::json NativeBridge::HandleUpdateSetting(const nlohmann::json& params) {
    auto& cfg = ConfigManager::Get().GetConfig();
    bool changed = false;
    if (params.contains("themeAccent")) {
        cfg.themeAccent = params["themeAccent"].get<std::string>();
        changed = true;
        BroadcastEvent("theme_changed", { {"theme", cfg.themeAccent} });
    }
    std::string key = params.value("key", "");
    if (!key.empty() && params.contains("value")) {
        if (key == "runInBackground") {
            cfg.runInBackground = params["value"].get<bool>();
            changed = true;
        } else if (key == "startMinimizedToTray") {
            cfg.startMinimizedToTray = params["value"].get<bool>();
            changed = true;
        } else if (key == "autostartWithWindows") {
            cfg.autostartWithWindows = params["value"].get<bool>();
            AutostartManager::Get().SetAutostart(cfg.autostartWithWindows);
            changed = true;
        } else if (key == "autoRamCleanEnabled") {
            cfg.autoRamCleanEnabled = params["value"].get<bool>();
            changed = true;
        } else if (key == "ramCleanIntervalMinutes") {
            cfg.ramCleanIntervalMinutes = params["value"].get<int>();
            changed = true;
        } else if (key == "ramThresholdPercent") {
            cfg.ramThresholdPercent = params["value"].get<int>();
            changed = true;
        } else if (key == "themeAccent") {
            cfg.themeAccent = params["value"].get<std::string>();
            changed = true;
            BroadcastEvent("theme_changed", { {"theme", cfg.themeAccent} });
        } else if (key == "gameBoostEnabled") {
            cfg.gameBoostEnabled = params["value"].get<bool>();
            changed = true;
        } else if (key == "boostHighPriority") {
            cfg.boostHighPriority = params["value"].get<bool>();
            changed = true;
        } else if (key == "boostStandbyPurge") {
            cfg.boostStandbyPurge = params["value"].get<bool>();
            changed = true;
        } else if (key == "boostLockTimer") {
            cfg.boostLockTimer = params["value"].get<bool>();
            changed = true;
        } else if (key == "autoCheckUpdates") {
            cfg.autoCheckUpdates = params["value"].get<bool>();
            changed = true;
        } else if (key == "updateChannel") {
            cfg.updateChannel = params["value"].get<std::string>();
            changed = true;
        }
    }
    if (changed) {
        ConfigManager::Get().Save();
    }

    nlohmann::json res;
    res["success"] = true;
    res["config"] = HandleGetConfig()["config"];
    return res;
}

// -----------------------------------------------------------------------------
// Windows 11 Tweaks Handlers
// -----------------------------------------------------------------------------
nlohmann::json NativeBridge::HandleGetWin11Tweaks() {
    Win11Tweaker::Get().DetectState();
    return Win11Tweaker::Get().GetStateJson();
}

nlohmann::json NativeBridge::HandleToggleWin11Tweak(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    bool enabled = params.value("enabled", params.value("value", false));
    std::string msg;
    bool ok = Win11Tweaker::Get().ToggleTweak(id, enabled, msg);
    ShowToast(msg, !ok);
    nlohmann::json res = Win11Tweaker::Get().GetStateJson();
    res["success"] = ok;
    res["message"] = msg;
    return res;
}

// -----------------------------------------------------------------------------
// Startup Manager Handlers
// -----------------------------------------------------------------------------
nlohmann::json NativeBridge::HandleGetStartupItems() {
    StartupManager::Get().ScanItems();
    return StartupManager::Get().GetItemsJson();
}

nlohmann::json NativeBridge::HandleScanStartup() {
    return HandleGetStartupItems();
}

nlohmann::json NativeBridge::HandleToggleStartupItem(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    bool enabled = params.value("enabled", params.value("value", false));
    return StartupManager::Get().ToggleItem(id, enabled);
}

nlohmann::json NativeBridge::HandleDeleteStartupItem(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    return StartupManager::Get().DeleteItem(id);
}

// -----------------------------------------------------------------------------
// MSI Interrupt Manager Handlers
// -----------------------------------------------------------------------------
nlohmann::json NativeBridge::HandleGetMsiDevices() {
    MsiManager::Get().ScanDevices();
    nlohmann::json res = MsiManager::Get().GetDevicesJson();
    res["success"] = true;
    return res;
}

nlohmann::json NativeBridge::HandleScanDevices() {
    return HandleGetMsiDevices();
}

nlohmann::json NativeBridge::HandleScanAppx() {
    SystemTweaker::Get().ScanInstalledAppxPackages();
    return HandleGetAppx();
}

nlohmann::json NativeBridge::HandleSetMsiEnabled(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    bool enabled = params.value("enabled", params.value("value", false));
    std::string msg;
    bool ok = MsiManager::Get().SetMsiEnabled(id, enabled, msg);
    ShowToast(msg, !ok);
    nlohmann::json res = MsiManager::Get().GetDevicesJson();
    res["success"] = ok;
    res["message"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleSetMsiPriority(const nlohmann::json& params) {
    std::string id = params.value("id", "");
    int priority = params.value("priority", 0);
    std::string msg;
    bool ok = MsiManager::Get().SetDevicePriority(id, priority, msg);
    ShowToast(msg, !ok);
    nlohmann::json res = MsiManager::Get().GetDevicesJson();
    res["success"] = ok;
    res["message"] = msg;
    return res;
}

nlohmann::json NativeBridge::HandleApplyMsiEsportsProfile() {
    std::string msg;
    bool ok = MsiManager::Get().ApplyRecommendedEsportsProfile(msg);
    ShowToast(msg, !ok);
    nlohmann::json res = MsiManager::Get().GetDevicesJson();
    res["success"] = ok;
    res["message"] = msg;
    return res;
}

// -----------------------------------------------------------------------------
// Game Boost Handlers
// -----------------------------------------------------------------------------
nlohmann::json NativeBridge::HandleGetGameBoostStatus() {
    return GameBoostManager::Get().GetStatusJson();
}

nlohmann::json NativeBridge::HandleUpdateGameBoostSettings(const nlohmann::json& params) {
    bool ok = GameBoostManager::Get().UpdateSettings(params);
    nlohmann::json res = GameBoostManager::Get().GetStatusJson();
    res["success"] = ok;
    return res;
}

nlohmann::json NativeBridge::HandleAddCustomBoostGame(const nlohmann::json& params) {
    std::string exe = params.value("exe", "");
    if (exe.empty()) exe = params.value("exeName", "");
    if (exe.empty()) exe = params.value("name", "");

    bool ok = GameBoostManager::Get().AddCustomGame(exe);
    nlohmann::json res = GameBoostManager::Get().GetStatusJson();
    res["success"] = ok;
    return res;
}

nlohmann::json NativeBridge::HandleRemoveCustomBoostGame(const nlohmann::json& params) {
    std::string exe = params.value("exe", "");
    if (exe.empty()) exe = params.value("exeName", "");
    if (exe.empty()) exe = params.value("name", "");

    bool ok = GameBoostManager::Get().RemoveCustomGame(exe);
    nlohmann::json res = GameBoostManager::Get().GetStatusJson();
    res["success"] = ok;
    return res;
}

nlohmann::json NativeBridge::HandleRemoveBoostGame(const nlohmann::json& params) {
    std::string gameId = params.value("id", "");
    std::string exe = params.value("exe", "");
    std::string name = params.value("name", "");
    if (exe.empty()) exe = params.value("exeName", "");
    if (gameId.empty()) gameId = exe;

    bool ok = GameBoostManager::Get().RemoveBoostGame(gameId, exe, name);
    nlohmann::json res;
    res["success"] = ok;
    return res;
}

nlohmann::json NativeBridge::HandleFetchGameCover(const nlohmann::json& params) {
    std::string name = params.value("name", "");
    std::string exe = params.value("exe", "");
    std::string path = params.value("path", "");
    uint32_t appId = 0;
    if (params.contains("appId")) {
        try { appId = params["appId"].get<uint32_t>(); } catch (...) {}
    }
    std::string cover = GameBoostManager::Get().FetchSteamCover(name, exe, appId, path);
    nlohmann::json res;
    res["success"] = !cover.empty() && cover != "/covers/default_game.jpg";
    res["cover"] = cover;
    return res;
}

// -----------------------------------------------------------------------------
// In-App Updates & GitHub Releases Handlers
// -----------------------------------------------------------------------------
nlohmann::json NativeBridge::HandleCheckUpdates(const nlohmann::json& params) {
    std::string channel = params.value("channel", "");
    UpdateInfo info = UpdateManager::Get().CheckUpdatesSync(channel);

    nlohmann::json res;
    res["success"] = info.errorMsg.empty();
    res["available"] = info.available;
    res["currentVersion"] = info.currentVersion;
    res["latestVersion"] = info.latestVersion;
    res["downloadUrl"] = info.downloadUrl;
    res["releaseUrl"] = info.releaseUrl;
    res["changelog"] = info.changelog;
    res["date"] = info.publishDate;
    res["channel"] = info.channel;
    res["error"] = info.errorMsg;

    if (info.available) {
        ShowToast("Доступно новое обновление: v" + info.latestVersion, false);
    } else if (!info.errorMsg.empty()) {
        ShowToast(info.errorMsg, true);
    } else {
        ShowToast("У вас установлена самая актуальная версия программы (v" + info.currentVersion + ")", false);
    }

    return res;
}

nlohmann::json NativeBridge::HandleGetUpdateInfo() {
    UpdateInfo info = UpdateManager::Get().GetLastUpdateInfo();
    nlohmann::json res;
    res["success"] = true;
    res["available"] = info.available;
    res["currentVersion"] = info.currentVersion;
    res["latestVersion"] = info.latestVersion;
    res["downloadUrl"] = info.downloadUrl;
    res["releaseUrl"] = info.releaseUrl;
    res["changelog"] = info.changelog;
    res["date"] = info.publishDate;
    res["channel"] = info.channel;
    res["error"] = info.errorMsg;
    return res;
}

nlohmann::json NativeBridge::HandleOpenUpdateUrl() {
    UpdateManager::Get().OpenDownloadUrl();
    nlohmann::json res;
    res["success"] = true;
    return res;
}




#include "restore_manager.h"
#include "tweaks_engine.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <thread>

#ifndef BEGIN_SYSTEM_CHANGE
#define BEGIN_SYSTEM_CHANGE 100
#endif
#ifndef END_SYSTEM_CHANGE
#define END_SYSTEM_CHANGE 101
#endif
#ifndef MODIFY_SETTINGS
#define MODIFY_SETTINGS 12
#endif

#pragma pack(push, 1)
typedef struct _RESTOREPOINTINFOW {
    DWORD dwEventType;
    DWORD dwRestorePtType;
    INT64 llSequenceNumber;
    WCHAR szDescription[MAX_PATH];
} RESTOREPOINTINFOW, *PRESTOREPOINTINFOW;

typedef struct _STATEMGRSTATUS {
    DWORD nStatus;
    INT64 llSequenceNumber;
} STATEMGRSTATUS, *PSTATEMGRSTATUS;
#pragma pack(pop)

typedef BOOL(WINAPI* pfnSRSetRestorePointW)(PRESTOREPOINTINFOW pRestorePointInfo, PSTATEMGRSTATUS pSMgrStatus);

RestoreManager& RestoreManager::Get() {
    static RestoreManager instance;
    return instance;
}

bool RestoreManager::CreateSystemRestorePoint(const std::wstring& description, std::string& outMessage) {
    HMODULE hSrClient = LoadLibraryW(L"srclient.dll");
    if (!hSrClient) {
        outMessage = "srclient.dll is not available on this system.";
        return false;
    }

    auto pfnSR = (pfnSRSetRestorePointW)GetProcAddress(hSrClient, "SRSetRestorePointW");
    if (!pfnSR) {
        FreeLibrary(hSrClient);
        outMessage = "SRSetRestorePointW API could not be located.";
        return false;
    }

    RESTOREPOINTINFOW rpBegin;
    ZeroMemory(&rpBegin, sizeof(rpBegin));
    rpBegin.dwEventType = BEGIN_SYSTEM_CHANGE;
    rpBegin.dwRestorePtType = MODIFY_SETTINGS;
    rpBegin.llSequenceNumber = 0;
    wcsncpy_s(rpBegin.szDescription, MAX_PATH, description.c_str(), _TRUNCATE);

    STATEMGRSTATUS smStatus;
    ZeroMemory(&smStatus, sizeof(smStatus));

    BOOL bRes = pfnSR(&rpBegin, &smStatus);
    if (!bRes || smStatus.nStatus != ERROR_SUCCESS) {
        FreeLibrary(hSrClient);
        if (smStatus.nStatus == 1058) { // Service disabled
            outMessage = "System Restore Service is disabled in Windows Settings.";
        } else {
            outMessage = "System Restore Point failed. Status code: " + std::to_string(smStatus.nStatus);
        }
        return false;
    }

    // End system change
    RESTOREPOINTINFOW rpEnd;
    ZeroMemory(&rpEnd, sizeof(rpEnd));
    rpEnd.dwEventType = END_SYSTEM_CHANGE;
    rpEnd.llSequenceNumber = smStatus.llSequenceNumber;

    pfnSR(&rpEnd, &smStatus);
    FreeLibrary(hSrClient);

    outMessage = "Restore Point successfully created!";
    return true;
}

void RestoreManager::CreateSystemRestorePointAsync(const std::wstring& description, std::function<void(bool success, const std::string& msg)> onComplete) {
    if (m_isBusy.load()) {
        if (onComplete) {
            onComplete(false, "Операция создания точки восстановления уже выполняется");
        }
        return;
    }

    m_isBusy.store(true);
    m_busyStatus = "Создание системной точки отката (SRSetRestorePointW)...";

    std::thread([this, description, onComplete]() {
        std::string msg;
        bool ok = CreateSystemRestorePoint(description, msg);
        m_isBusy.store(false);
        m_busyStatus = ok ? "Точка восстановления успешно создана" : "Ошибка создания точки восстановления";
        if (onComplete) {
            onComplete(ok, msg);
        }
    }).detach();
}

bool RestoreManager::ExportProfile(const std::wstring& filePath, std::string& outMessage) {
    std::ofstream file(filePath);
    if (!file.is_open()) {
        outMessage = "Failed to open file for writing.";
        return false;
    }

    const auto& s = TweaksEngine::Get().GetState();
    file << "[WCS_PROFILE_V1]\n";
    file << "timer_05ms=" << (s.timerResolution05ms ? 1 : 0) << "\n";
    file << "disable_dynamic_tick=" << (s.disableDynamicTick ? 1 : 0) << "\n";
    file << "use_platform_clock_no=" << (s.usePlatformClockNo ? 1 : 0) << "\n";
    file << "use_platform_tick_no=" << (s.usePlatformTickNo ? 1 : 0) << "\n";
    file << "win32_priority=" << (s.win32PrioritySeparation ? 1 : 0) << "\n";
    file << "ultimate_power=" << (s.ultimatePowerPlan ? 1 : 0) << "\n";
    file << "core_unpark=" << (s.disableCoreParking ? 1 : 0) << "\n";
    file << "gpu_msi=" << (s.gpuMsiMode ? 1 : 0) << "\n";
    file << "audio_msi=" << (s.audioMsiMode ? 1 : 0) << "\n";
    file << "hags_enabled=" << (s.hagsEnabled ? 1 : 0) << "\n";
    file << "csrss_dwm_priority=" << (s.csrssDwmPriority ? 1 : 0) << "\n";
    file << "disable_game_dvr=" << (s.disableGameDVR ? 1 : 0) << "\n";
    file << "disable_fso=" << (s.disableFullscreenOptimizations ? 1 : 0) << "\n";
    file << "mmcss_games=" << (s.mmcssGamesPriority ? 1 : 0) << "\n";
    file << "network_throttling=" << (s.disableNetworkThrottling ? 1 : 0) << "\n";
    file << "tcp_nodelay=" << (s.tcpNoDelay ? 1 : 0) << "\n";
    file << "tcp_autotuning=" << (s.tcpAutoTuning ? 1 : 0) << "\n";
    file << "disable_paging_exec=" << (s.disablePagingExecutive ? 1 : 0) << "\n";
    file << "large_sys_cache_dis=" << (s.largeSystemCacheDisabled ? 1 : 0) << "\n";
    file << "flat_mouse=" << (s.flatMouseCurve ? 1 : 0) << "\n";
    file << "keyboard_speed=" << (s.keyboardMaxSpeed ? 1 : 0) << "\n";
    file << "disable_telemetry=" << (s.disableTelemetry ? 1 : 0) << "\n";
    file << "disable_wer=" << (s.disableErrorReporting ? 1 : 0) << "\n";

    file.close();
    outMessage = "Profile exported successfully!";
    return true;
}

bool RestoreManager::ImportProfile(const std::wstring& filePath, std::string& outMessage) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        outMessage = "Failed to open profile file.";
        return false;
    }

    auto& s = TweaksEngine::Get().GetMutableState();
    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        int val = std::stoi(line.substr(eq + 1));
        bool bVal = (val == 1);

        if (key == "timer_05ms") s.timerResolution05ms = bVal;
        else if (key == "disable_dynamic_tick") s.disableDynamicTick = bVal;
        else if (key == "use_platform_clock_no") s.usePlatformClockNo = bVal;
        else if (key == "use_platform_tick_no") s.usePlatformTickNo = bVal;
        else if (key == "win32_priority") s.win32PrioritySeparation = bVal;
        else if (key == "ultimate_power") s.ultimatePowerPlan = bVal;
        else if (key == "core_unpark") s.disableCoreParking = bVal;
        else if (key == "gpu_msi") s.gpuMsiMode = bVal;
        else if (key == "audio_msi") s.audioMsiMode = bVal;
        else if (key == "hags_enabled") s.hagsEnabled = bVal;
        else if (key == "csrss_dwm_priority") s.csrssDwmPriority = bVal;
        else if (key == "disable_game_dvr") s.disableGameDVR = bVal;
        else if (key == "disable_fso") s.disableFullscreenOptimizations = bVal;
        else if (key == "mmcss_games") s.mmcssGamesPriority = bVal;
        else if (key == "network_throttling") s.disableNetworkThrottling = bVal;
        else if (key == "tcp_nodelay") s.tcpNoDelay = bVal;
        else if (key == "tcp_autotuning") s.tcpAutoTuning = bVal;
        else if (key == "disable_paging_exec") s.disablePagingExecutive = bVal;
        else if (key == "large_sys_cache_dis") s.largeSystemCacheDisabled = bVal;
        else if (key == "flat_mouse") s.flatMouseCurve = bVal;
        else if (key == "keyboard_speed") s.keyboardMaxSpeed = bVal;
        else if (key == "disable_telemetry") s.disableTelemetry = bVal;
        else if (key == "disable_wer") s.disableErrorReporting = bVal;
    }
    file.close();

    std::string applyMsg;
    TweaksEngine::Get().ApplyAll(applyMsg);
    outMessage = "Profile imported and applied!";
    return true;
}

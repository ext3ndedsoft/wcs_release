#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

enum class PresetType {
    None,
    EsportsUltra,
    CompetitiveGaming,
    BalancedGaming = CompetitiveGaming,
    WindowsDefault
};

struct TweakState {
    // 1. Latency & CPU
    bool timerResolution05ms = false;
    bool disableDynamicTick = false;
    bool usePlatformClockNo = false;
    bool usePlatformTick = false;
    bool usePlatformTickNo = false; // alias for compatibility
    bool bootuxDisabled = false;
    bool win32PrioritySeparation = false;
    DWORD win32PriorityValue = 0x26;
    bool ultimatePowerPlan = false;
    bool disableCoreParking = false;
    bool powerThrottling = false;

    // 2. GPU & Graphics
    bool gpuMsiMode = false;
    bool audioMsiMode = false;
    bool hagsEnabled = false;
    bool forceP0 = false;
    bool resizableBarDetected = false;
    bool csrssDwmPriority = false;
    bool disableGameDVR = false;
    bool disableFullscreenOptimizations = false;

    // 3. MMCSS & Priorities
    bool mmcssGamesPriority = false;
    bool disableNetworkThrottling = false;

    // 4. Network & Ping
    bool tcpNoDelay = false;
    bool tcpAckFrequency = false;
    bool tcpAutoTuning = false;
    bool defaultTTL64 = false;
    bool optimalMTU = false;

    // 5. Memory Management
    bool disablePagingExecutive = false;
    bool largeSystemCacheDisabled = false;
    bool ioPageLockLimit = false;

    // 6. Input & Debloat
    bool flatMouseCurve = false;
    bool keyboardMaxSpeed = false;
    bool disableTelemetry = false;
    bool disableErrorReporting = false;
};

class TweaksEngine {
public:
    static TweaksEngine& Get();

    // Query and update live tweak states from registry and system
    void DetectFastState();
    void DetectCurrentState();
    void DetectCurrentStateAsync(std::function<void()> onComplete = nullptr);

    TweakState GetState() const {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        return m_state;
    }
    TweakState& GetMutableState() { return m_state; }

    bool IsInitialized() const { return m_isInitialized.load(); }
    bool IsBcdReady() const { return m_bcdReady.load(); }

    // Individual Tweak Handlers
    bool SetTimerResolution(bool enable);
    bool SetDynamicTick(bool disable);
    bool SetPlatformClock(bool disableHPET);
    bool SetPlatformTick(bool enable);
    bool SetBootUX(bool disable);
    bool SetWin32PrioritySeparation(bool enable0x26, DWORD customVal = 0x26);
    bool SetUltimatePowerPlan(bool enable);
    bool SetCoreParking(bool disable);
    bool SetPowerThrottling(bool disable);

    bool SetGPUMsiMode(bool enable);
    bool SetAudioMsiMode(bool enable);
    bool SetHAGS(bool enable);
    bool SetForceP0(bool enable);
    static bool DetectResizableBar();
    bool SetCsrssDwmPriority(bool enable);
    bool SetGameDVR(bool disable);
    bool SetFullscreenOptimizations(bool disable);

    bool SetMMCSSGamesPriority(bool enable);
    bool SetNetworkThrottling(bool disable);

    bool SetTCPNoDelay(bool enable);
    bool SetTCPAckFrequency(bool enable);
    bool SetTCPAutoTuning(bool enable);
    bool SetDefaultTTL(bool enable64);
    bool SetNetworkMTU(bool enable1500);
    bool FlushDNS();

    bool SetDisablePagingExecutive(bool enable);
    bool SetLargeSystemCache(bool gamingMode);
    bool SetIoPageLockLimit(bool enable);

    bool SetFlatMouseCurve(bool enable);
    bool SetKeyboardSpeed(bool enable);
    bool SetTelemetry(bool disable);
    bool SetErrorReporting(bool disable);

    // Standby Memory Purge (Zero external tools, pure NT API)
    bool PurgeStandbyMemory(std::string& outMsg);

    // Apply entire active state to system
    bool ApplyAll(std::string& outStatus);

    // Apply standard preset
    bool ApplyPreset(PresetType preset, std::string& outStatus);
    bool ApplyPreset(const std::string& presetName, std::string& outStatus);

    // Revert all modified settings to Microsoft default values
    bool RevertToDefaults(std::string& outStatus);

    PresetType GetActivePreset() const { return m_activePreset; }
    void SetActivePreset(PresetType p) { m_activePreset = p; }
    PresetType EvaluateActivePreset() const;

private:
    TweaksEngine();
    ~TweaksEngine() = default;

    mutable std::recursive_mutex m_stateMutex;
    std::atomic<bool> m_isInitialized{ false };
    std::atomic<bool> m_bcdReady{ false };

    TweakState m_state;
    PresetType m_activePreset = PresetType::None;

    // Registry helper functions
    static bool RegWriteDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, DWORD data);
    static bool RegReadDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, DWORD& outData);
    static bool RegWriteString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, const std::wstring& data);
    static bool RegReadString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, std::wstring& outData);
    static bool RegWriteBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, const BYTE* data, DWORD size);
    static bool RegReadBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, BYTE* outData, DWORD& inOutSize);
    static bool RegDeleteValue(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName);

    // PCI MSI detection helper
    static bool DetectPciMsiMode(const wchar_t* targetClassGuid);

    // Command runner helpers
    static bool RunCommandHidden(const std::wstring& cmd);
    static bool RunCommandCapture(const std::wstring& cmd, std::string& outStdOut);
};

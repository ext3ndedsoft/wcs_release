#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

// -----------------------------------------------------------------------------
// SYSTEM SUITE STATE STRUCTURES
// -----------------------------------------------------------------------------

// 1. Система (System Tweaks)
struct SystemTweaksConfig {
    bool maxOsPerf = false;                 // Visual performance & responsiveness
    bool maxSsdPerf = false;                // SSD performance (8.3 names, NTFS access time)
    bool disablePowerThrottling = false;    // Disable Intel/AMD Power Throttling (PowerThrottlingOff = 1)
    bool disableHibernation = false;        // Disable Hibernation & Fast Startup (powercfg -h off)
    bool enableF8Boot = false;              // F8 legacy boot menu
    bool classicPhotoViewer = true;         // Classic Windows Photo Viewer
    bool disableStickyKeys = true;          // Disable Sticky / Filter / Toggle Keys
    bool restartShellOnCrash = true;        // Auto restart Explorer shell on crash
    bool svchostGrouping = true;            // SvcHost process grouping threshold
    bool disableRemoteAssistance = false;   // Disable Remote Assistance
    bool allowNetFramework4 = false;        // .NET 4 strong crypto & compatibility
    bool autoEndTasks = false;              // Auto end tasks on shutdown
    bool reduceHungAppTimeout = false;      // Reduce hung app / service kill timeouts
    bool autoKillHungApps = true;           // Auto kill hung apps
    bool disablePersistBrowsers = false;    // Don't reopen previous apps on reboot
    bool disableFinishSetup = false;        // Disable "Finish setting up device" OOBE
    bool verboseStatus = false;             // Verbose boot/shutdown status messages
    bool hideUserAccountsOnLogon = false;   // Hide user list on logon screen
    bool requireCtrlAltDel = false;         // Require Ctrl+Alt+Del before logon
    bool enableMsiSafeMode = false;         // Allow Windows Installer in Safe Mode
    bool disableAllEventLogs = false;       // Disable event logs / tracing
    bool disableBsodAutoRestart = false;    // Disable auto reboot on BSOD
    bool powerShellAcceleration = false;    // Accelerate PowerShell script execution
};

// 2. Безопасность (Security & Telemetry)
struct SecurityTweaksConfig {
    bool disableAllTelemetry = false;       // Master Windows & Office telemetry
    bool disableHiddenMonitoring = false;   // Disable Activity History & Timeline
    bool removeSpyServices = false;         // Disable DiagTrack, dmwappushservice
    bool blockTelemetryDomains = false;     // Block telemetry domains via hosts
    bool disableSecurityCenterNotifications = true; // Disable SecHealth toasts
    bool disableExeWarnings = false;        // Disable "Open File - Security Warning"
    bool disableDownloadWarnings = false;   // Disable attachment mark-of-the-web
    bool disableBrowserWarnings = false;    // Disable browser download warnings
    bool disableUAC = true;                 // Disable UAC (EnableLUA = 0)
    bool disableUsbAutorun = false;         // Disable USB AutoRun / AutoPlay
    bool disableCpuMitigations = false;     // Disable Meltdown/Spectre/Downfall for FPS
    bool disableVbsHvci = false;            // Disable VBS & HVCI Core Isolation (HypervisorEnforcedCodeIntegrity = 0)
    bool blockFacebookSync = false;         // Disable Facebook contact sync
    bool blockOutlookSync = false;          // Disable Outlook contact sync
    bool blockSkypeSync = false;            // Disable Skype contact sync
    bool preventEdgeAutorun = false;        // Disable MS Edge background prelaunch
    bool hideRealTTL = false;               // Hide real TTL (DefaultTTL = 65)
};

// 2.1 Дополнительные опции: Максимальная производительность ОС (12 опций)
struct OsPerfSubTweaks {
    bool waitScreenLock = false;          // Ожидание блокировки экрана
    bool noMonitorOff = false;            // Не выключать монитор
    bool noDiskSpindown = false;          // Не выключать диски
    bool ultimatePowerPlan = false;       // Максимальная схема питания
    bool alwaysUnloadDll = false;         // Автоматическая выгрузка DLL из памяти
    bool nonPagedPoolFix = false;         // Исправление утечки памяти (невыгружаемого пула)
    bool disableBackgroundApps = false;   // Отключить работу приложений в фоновом режиме
    bool disableAutoWindowsUpdate = false;// Отключить автоматическое обновление системы
    bool disableDriverUpdates = false;    // Отключить автоматическое обновление драйверов
    bool disableReservedStorage = false;  // Отключить зарезервированное хранилище Windows
    bool disableTurboBoostAC = false;     // Отключить Turbo Boost (для питания от сети)
    bool disableTurboBoostDC = false;     // Отключить Turbo Boost (для питания от батареи)
    bool disablePowerThrottling = false;  // Отключить Power Throttling
    bool disableHibernation = false;      // Отключить гибернацию и быстрый запуск
};

// 2.2 Дополнительные опции: Максимальная производительность SSD (8 опций)
struct SsdPerfSubTweaks {
    bool disablePrefetchSuperfetch = false; // Отключить Prefetcher и Superfetch
    bool disableBootDefrag = false;          // Отключить дефрагментацию системных файлов
    bool disableLastAccess = false;          // Отключить время последнего доступа к файлу
    bool disable8dot3Names = false;          // Отключить создание коротких имен 8.3
    bool enableTrimSupport = true;           // Включить поддержку функции TRIM
    bool disableLowDiskSpaceChecks = false;  // Отключить сообщения о нехватке места на диске
    bool disablePagingExecutive = false;     // Оставлять ядро системы в памяти
    bool largeSystemCache = false;           // Увеличить размер кэша файловой системы
    bool win32PrioritySeparation = false;    // Улучшить производительность приложений
};

// 2.3 Дополнительные опции: Все виды телеметрий Windows и MSOffice (15 опций)
struct TelemetrySubTweaks {
    bool disableAdvertisingId = false;      // Отключить рекламный идентификатор и рекламы
    bool disableSettingSync = false;        // Отключить все типы синхронизаций Windows
    bool disableVsTelemetry = false;        // Отключить все телеметрии Windows и Visual Studio
    bool disableOfficeTelemetry = false;    // Отключить все телеметрии Microsoft Office
    bool disableNvidiaTelemetry = false;    // Отключить телеметрию NVIDIA
    bool disableCeipTasks = false;          // Отключить сбор данных через события планировщика
    bool disableAppInventory = false;       // Отключить сбор данных об установленных приложениях
    bool disableAppUsageStats = false;      // Отключить сбор статистики использования приложений
    bool disableInkingTypingData = false;   // Отключить сбор и отправку данных рукописного ввода
    bool disableUserBehavior = false;       // Отключить ведение записи поведения пользователя
    bool disableLocation = false;           // Отключить определение местоположения пользователя
    bool disableFeedbackFrequency = false;  // Отключить проверку обращений через Обратную связь
    bool disableSpeechModelUpdate = false;  // Отключить скрытое фоновое обновление синтеза речи
    bool disableExperiments = false;        // Отключить эксперименты со стороны Microsoft
    bool disableAutoLoggerDiagTrack = false;// Отключить сбор и обработку данных Журнала событий
};

// 3. Службы (Services)
struct ServiceItemInfo {
    std::string id;
    std::string displayName;
    std::string description;
    std::vector<std::wstring> winServiceNames;
    DWORD defaultStartType;                 // SERVICE_AUTO_START, SERVICE_DEMAND_START
    bool recommendedDisable;                // Esports recommended disabled
    bool isDanger;                          // Danger / red service
    bool disabled;                          // Current user toggle state
    std::string statusText = "Остановлена"; // "Работает", "Остановлена", "Отключена", "Включена"
    DWORD currentServiceState = SERVICE_STOPPED; // SERVICE_RUNNING, SERVICE_STOPPED, etc.
    bool isRunning = false;                 // Live runtime state
    std::string status = "Остановлена";     // Live display status: "Работает", "Включена", "Отключена"
};

// 4. Приложения (UWP Debloat)
struct AppxItemInfo {
    std::string id;
    std::string displayName;
    std::string packageNamePattern;         // e.g. "*ZuneMusic*"
    std::string category;                   // "Развлечения", "Браузеры", etc.
    bool selected = false;                  // Selected for uninstall
    bool isInstalled = false;               // Detected state on the current system (verified via registry / scan)
};

// 5. Очистка системы (Cleanup Items)
struct CleanupOptions {
    bool cleanDefenderCache = false;
    bool cleanUsbHistory = false;
    bool cleanWindowsInstallCache = false;
    bool cleanEventLogs = false;
    bool cleanSoftwareDistribution = false;
    bool cleanTempFiles = false;
    bool cleanExplorerThumbnails = false;
    bool cleanDiagnosticLogs = false;
    bool cleanBrowserCache = false;
    bool cleanSystemReports = false;
    bool cleanOfficeCache = false;
    bool cleanGpuCache = false;
};

// -----------------------------------------------------------------------------
// SystemTweaker Controller Singleton
// -----------------------------------------------------------------------------
class SystemTweaker {
public:
    static SystemTweaker& Get();

    void Init();
    void DetectFastState();
    void InitAsync(std::function<void()> onComplete = nullptr);

    bool IsInitialized() const { return m_isInitialized.load(); }
    bool IsServicesScanned() const { return m_servicesScanned.load(); }
    bool IsAppxScanned() const { return m_appxScanned.load(); }
    bool IsSsdScanned() const { return m_ssdScanned.load(); }

    // Live state getters & mutables
    SystemTweaksConfig& GetSystemConfig() { return m_sysConfig; }
    const SystemTweaksConfig& GetSystemConfig() const { return m_sysConfig; }

    SecurityTweaksConfig& GetSecurityConfig() { return m_secConfig; }
    const SecurityTweaksConfig& GetSecurityConfig() const { return m_secConfig; }

    OsPerfSubTweaks& GetOsPerfSubTweaks() { return m_osPerfSub; }
    const OsPerfSubTweaks& GetOsPerfSubTweaks() const { return m_osPerfSub; }

    SsdPerfSubTweaks& GetSsdPerfSubTweaks() { return m_ssdPerfSub; }
    const SsdPerfSubTweaks& GetSsdPerfSubTweaks() const { return m_ssdPerfSub; }

    TelemetrySubTweaks& GetTelemetrySubTweaks() { return m_telemetrySub; }
    const TelemetrySubTweaks& GetTelemetrySubTweaks() const { return m_telemetrySub; }

    std::vector<ServiceItemInfo>& GetServices() { return m_services; }
    const std::vector<ServiceItemInfo>& GetServices() const { return m_services; }
    std::vector<ServiceItemInfo>& GetServiceList() { return m_services; }
    const std::vector<ServiceItemInfo>& GetServiceList() const { return m_services; }

    std::vector<AppxItemInfo>& GetAppxItems() { return m_appxItems; }
    const std::vector<AppxItemInfo>& GetAppxItems() const { return m_appxItems; }

    CleanupOptions& GetCleanupOptions() { return m_cleanupOptions; }
    const CleanupOptions& GetCleanupOptions() const { return m_cleanupOptions; }

    // UWP header options
    bool disableStoreAutoUpdate = false;
    bool disableStartRecommended = true;

    // Apply Methods
    bool ApplySystemTweaks(std::string& outMsg);
    bool ApplySecurityTweaks(std::string& outMsg);
    bool ApplySingleSystemTweak(const std::string& tweakId, bool value, std::string& outMsg);
    bool ApplySingleSecurityTweak(const std::string& tweakId, bool value, std::string& outMsg);

    // Sub-tweak flyout and state detection methods
    void DetectSystemState();
    void DetectSecurityState();
    void DetectOsPerfSubState();
    void DetectSsdPerfSubState();
    void DetectTelemetrySubState();

    bool ApplyOsPerfSubTweak(const std::string& tweakId, bool value, std::string& outMsg);
    bool ApplyAllOsPerfSubTweaks(bool enable, std::string& outMsg);

    bool ApplySsdPerfSubTweak(const std::string& tweakId, bool value, std::string& outMsg);
    bool ApplyAllSsdPerfSubTweaks(bool enable, std::string& outMsg);

    bool ApplyTelemetrySubTweak(const std::string& tweakId, bool value, std::string& outMsg);
    bool ApplyAllTelemetrySubTweaks(bool enable, std::string& outMsg);

    // Services Methods
    void RefreshServiceStates();
    bool ApplyServiceState(ServiceItemInfo& item, bool disable);
    bool EnableAllServices(std::string& outMsg);
    bool DisableRecommendedServices(std::string& outMsg);
    int GetTotalServicesCount() const { return (int)m_services.size(); }
    int GetDisabledServicesCount() const;

    // AppX Methods
    void ScanInstalledAppxPackages();
    bool UninstallSelectedAppx(const std::function<void(const std::string&)>& progressCb, std::string& outMsg);
    bool UninstallAllAppx(const std::function<void(const std::string&)>& progressCb, std::string& outMsg);
    bool RestoreAppxPackage(const std::string& appId, std::string& outMsg);
    void SelectAllAppx(bool select);
    bool ApplyAppxSettings(std::string& outMsg);

    // Cleanup Methods
    void SetAllCleanupOptions(bool set);
    bool ExecuteCleanup(const CleanupOptions& opts, uint64_t& outBytesFreed, std::string& outMsg);

    // Bottom Action Buttons from Screenshot 5
    bool TerminateBackgroundProcesses(std::string& outMsg);
    bool CleanRamWorkingSets(std::string& outMsg);
    bool CleanJunkFolders(std::string& outMsg);
    bool CleanRegistryJunk(std::string& outMsg);
    bool CleanComponentStoreWinSxS(std::string& outMsg);
    bool CleanDriverStore(std::string& outMsg);
    bool RevertAllTweaks(std::string& outMsg);

    // Async worker status
    bool IsBusy() const { return m_isBusy.load(); }
    std::string GetBusyStatus() const { return m_busyStatus; }

    static bool RunCmdSilent(const std::wstring& cmd, DWORD timeoutMs = 15000);
    static bool RunCmdWithOutput(const std::wstring& cmd, std::string& outOutput, DWORD timeoutMs = 15000);

private:
    SystemTweaker();
    ~SystemTweaker() = default;

    void InitServiceDatabase();
    void InitAppxDatabase();

    SystemTweaksConfig m_sysConfig;
    SecurityTweaksConfig m_secConfig;
    OsPerfSubTweaks m_osPerfSub;
    SsdPerfSubTweaks m_ssdPerfSub;
    TelemetrySubTweaks m_telemetrySub;
    std::vector<ServiceItemInfo> m_services;
    std::vector<AppxItemInfo> m_appxItems;
    CleanupOptions m_cleanupOptions;

    std::atomic<bool> m_isBusy{ false };
    std::string m_busyStatus;

    mutable std::mutex m_servicesMutex;
    mutable std::mutex m_appxMutex;
    mutable std::mutex m_configMutex;

    std::atomic<bool> m_isInitialized{ false };
    std::atomic<bool> m_servicesScanned{ false };
    std::atomic<bool> m_appxScanned{ false };
    std::atomic<bool> m_ssdScanned{ false };

    // Internal Registry & Win32 Helpers
    static bool RegSetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD val);
    static bool RegSetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, const std::wstring& val);
    static bool RegDeleteVal(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName);
    static DWORD RegGetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD defaultVal = 0);
    static std::wstring RegGetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName);
    static uint64_t DeleteDirectoryFiles(const std::wstring& dirPath, bool recursive = true);
    static uint64_t DeleteDirectoriesParallel(const std::vector<std::wstring>& paths, bool recursive = true);
};

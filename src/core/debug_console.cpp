#include "debug_console.h"
#include "system_info.h"
#include "timer_resolution.h"
#include "memory_cleaner.h"
#include "tweaks_engine.h"
#include "system_tweaker.h"
#include "debug_logger.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <conio.h>

namespace {

HANDLE g_hConsole = INVALID_HANDLE_VALUE;

void InitConsole() {
    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hConsole == NULL || g_hConsole == INVALID_HANDLE_VALUE) {
        AllocConsole();
        g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"Windows Config Studio [DIAGNOSTIC CONSOLE] — System Audit & Latency Optimizer");

    DWORD mode = 0;
    if (GetConsoleMode(g_hConsole, &mode)) {
        SetConsoleMode(g_hConsole, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

enum ConsoleCol {
    ColReset   = 7,
    ColDim     = 8,
    ColBlue    = 9,
    ColGreen   = 10,
    ColCyan    = 11,
    ColRed     = 12,
    ColMagenta = 13,
    ColYellow  = 14,
    ColWhite   = 15
};

void Print(ConsoleCol col, const std::string& str) {
    DWORD written = 0;
    if (g_hConsole != INVALID_HANDLE_VALUE && g_hConsole != NULL) {
        SetConsoleTextAttribute(g_hConsole, (WORD)col);
        if (!WriteConsoleA(g_hConsole, str.data(), (DWORD)str.size(), &written, NULL)) {
            WriteFile(g_hConsole, str.data(), (DWORD)str.size(), &written, NULL);
        }
        SetConsoleTextAttribute(g_hConsole, ColReset);
    } else {
        std::cout.write(str.data(), str.size());
        std::cout.flush();
    }
}

void PrintLn(ConsoleCol col, const std::string& str) {
    Print(col, str + "\n");
}

void PrintHeader() {
    PrintLn(ColCyan, "==================================================================================");
    PrintLn(ColWhite, "   WINDOWS CONFIG STUDIO — STANDALONE DEBUG & DIAGNOSTIC CONSOLE");
    PrintLn(ColDim,   "   Direct Win32 Hardware Telemetry, Tweak Auditing & Live Latency Verification");
    PrintLn(ColCyan, "==================================================================================");
}

void PrintSpecsDashboard() {
    SystemInfo::Get().Refresh();
    const auto& sp = SystemInfo::Get().GetSpecs();
    DetailedMemoryStats mem = MemoryCleaner::Get().QueryDetailedStats();

    PrintLn(ColYellow, "\n--- [ LIVE HARDWARE & SYSTEM TELEMETRY ] -----------------------------------------");

    std::ostringstream ss;
    ss << "  [OS]        " << sp.osName << " " << sp.osBuild << "\n";
    ss << "  [CPU]       " << sp.cpuName << " (" << sp.physicalCores << " Cores, " << sp.logicalCores << " Threads)\n";
    ss << "  [RAM]       " << std::fixed << std::setprecision(2)
       << mem.usedRAM_GB << " GB used / " << mem.totalRAM_GB << " GB total (" 
       << (int)mem.usedPercent << "% load, Standby Cache: " << mem.standbyRAM_GB << " GB)\n";

    if (!sp.primaryGPUName.empty()) {
        ss << "  [GPU]       " << sp.primaryGPUName << " [MSI: " 
           << (sp.primaryGPUMSIEnabled ? "ENABLED, High Priority" : "DISABLED (Line-Based)") << "]\n";
    }

    ss << "  [POWER]     " << sp.activePowerPlan << "\n";
    ss << "  [TIMER]     Current: " << std::fixed << std::setprecision(3) << sp.currentTimerResolutionMs 
       << " ms  (Min: " << sp.minTimerResolutionMs << " ms, Max: " << sp.maxTimerResolutionMs << " ms)\n";

    Print(ColWhite, ss.str());
    PrintLn(ColDim, "----------------------------------------------------------------------------------");
}

void PrintAuditRow(const std::string& name, bool applied, const std::string& detail) {
    if (applied) {
        Print(ColGreen, "  [✓] [APPLIED] ");
    } else {
        Print(ColDim,   "  [ ] [DEFAULT] ");
    }
    Print(ColWhite, name);
    if (!detail.empty()) {
        Print(ColDim, " -> " + detail);
    }
    PrintLn(ColReset, "");
}

void RunLiveTweakAudit() {
    PrintLn(ColYellow, "\n=== [ 1. LATENCY & KERNEL TWEAKS AUDIT (TweaksEngine) ] ==========================");
    TweaksEngine::Get().DetectCurrentState();
    const auto& ts = TweaksEngine::Get().GetState();

    PrintAuditRow("Timer Resolution 0.500 ms", ts.timerResolution05ms, "NtSetTimerResolution service active");
    PrintAuditRow("Dynamic Tick Disabled", ts.disableDynamicTick, "bcdedit /set disabledynamictick yes");
    PrintAuditRow("Platform Clock HPET Disabled", ts.usePlatformClockNo, "bcdedit /set useplatformclock no");
    PrintAuditRow("Platform Tick HPET Disabled", ts.usePlatformTickNo, "bcdedit /set useplatformtick no (APIC local timer)");
    PrintAuditRow("Win32 Priority Separation (0x26)", ts.win32PrioritySeparation, "PriorityControl Win32PrioritySeparation=38 (0x26 - 3:1 Boost)");
    PrintAuditRow("Ultimate Power Scheme", ts.ultimatePowerPlan, "Active power plan = Ultimate Performance");
    PrintAuditRow("CPU Core Parking Disabled", ts.disableCoreParking, "CPMINCORES = 100%, PROCTHROTTLEMIN = 100%");
    PrintAuditRow("GPU MSI Mode (Message Signaled)", ts.gpuMsiMode, "PCI Interrupt Management MSISupported=1");
    PrintAuditRow("Audio MSI Mode", ts.audioMsiMode, "Audio Controller MSISupported=1");
    PrintAuditRow("Hardware GPU Scheduling (HAGS)", ts.hagsEnabled, "GraphicsDrivers HwSchMode=2");
    PrintAuditRow("CSRSS & DWM High Priority", ts.csrssDwmPriority, "IFEO PerfOptions CpuPriorityClass=3 (High)");
    PrintAuditRow("GameDVR & Game Bar Disabled", ts.disableGameDVR, "GameConfigStore GameDVR_Enabled=0");
    PrintAuditRow("Fullscreen Optimizations Disabled", ts.disableFullscreenOptimizations, "GameDVR_DSEBehavior=1");
    PrintAuditRow("MMCSS Games Priority Scheduling", ts.mmcssGamesPriority, "SystemResponsiveness=0, GPU Priority=8");
    PrintAuditRow("Network Throttling Disabled", ts.disableNetworkThrottling, "SystemProfile NetworkThrottlingIndex=0xFFFFFFFF");
    PrintAuditRow("TCP NoDelay & AckFrequency (1)", ts.tcpNoDelay, "Tcpip Parameters Interfaces TcpAckFrequency=1");
    PrintAuditRow("TCP AutoTuning Level Normal", ts.tcpAutoTuning, "netsh int tcp global autotuninglevel=normal");
    PrintAuditRow("Kernel Paging Locked in RAM", ts.disablePagingExecutive, "Memory Management DisablePagingExecutive=1");
    PrintAuditRow("Gaming File Cache Mode (0)", ts.largeSystemCacheDisabled, "Memory Management LargeSystemCache=0");
    PrintAuditRow("1:1 Linear Flat Mouse Curve (MarkC)", ts.flatMouseCurve, "SmoothMouseX/YCurve flat linear table");
    PrintAuditRow("Keyboard Repeat Rate (Delay 0, Speed 31)", ts.keyboardMaxSpeed, "Control Panel KeyboardDelay=0, Speed=31");
    PrintAuditRow("Windows Diagnostic Telemetry Blocked", ts.disableTelemetry, "DataCollection AllowTelemetry=0");
    PrintAuditRow("Windows Error Reporting Disabled", ts.disableErrorReporting, "Windows Error Reporting Disabled=1");

    PrintLn(ColYellow, "\n=== [ 2. OS PERFORMANCE SUB-TWEAKS AUDIT (12 Items) ] ============================");
    SystemTweaker::Get().DetectOsPerfSubState();
    const auto& os = SystemTweaker::Get().GetOsPerfSubTweaks();

    PrintAuditRow("Ожидание блокировки экрана", os.waitScreenLock, "ScreenSaveActive=0, NoLockScreen=1, Standby Timeout=0");
    PrintAuditRow("Не выключать монитор", os.noMonitorOff, "Power Schemes SUB_VIDEO VIDEOIDLE AC=0");
    PrintAuditRow("Не выключать диски", os.noDiskSpindown, "Power Schemes SUB_DISK DISKIDLE AC=0");
    PrintAuditRow("Максимальная схема питания", os.ultimatePowerPlan, "Ultimate Performance power scheme active");
    PrintAuditRow("Автоматическая выгрузка DLL", os.alwaysUnloadDll, "Explorer AlwaysUnloadDLL=1");
    PrintAuditRow("Фикс утечки памяти пула", os.nonPagedPoolFix, "Memory Management PoolUsageMaximum=60");
    PrintAuditRow("Отключить фоновые UWP приложения", os.disableBackgroundApps, "BackgroundAccessApplications GlobalUserDisabled=1");
    PrintAuditRow("Отключить автообновление Windows", os.disableAutoWindowsUpdate, "WindowsUpdate AU NoAutoUpdate=1");
    PrintAuditRow("Отключить обновление драйверов", os.disableDriverUpdates, "WindowsUpdate ExcludeWUDriversInQualityUpdate=1");
    PrintAuditRow("Отключить зарезервированное хранилище", os.disableReservedStorage, "ReserveManager ShippedWithReserves=0 (7+ GB)");
    PrintAuditRow("Отключить Turbo Boost (от сети)", os.disableTurboBoostAC, "SUB_PROCESSOR PERFBOOSTMODE AC=0");
    PrintAuditRow("Отключить Turbo Boost (от батареи)", os.disableTurboBoostDC, "SUB_PROCESSOR PERFBOOSTMODE DC=0");

    PrintLn(ColYellow, "\n=== [ 3. SSD PERFORMANCE SUB-TWEAKS AUDIT (8 Items) ] ============================");
    SystemTweaker::Get().DetectSsdPerfSubState();
    const auto& ssd = SystemTweaker::Get().GetSsdPerfSubTweaks();

    PrintAuditRow("Отключить Prefetcher и Superfetch", ssd.disablePrefetchSuperfetch, "EnablePrefetcher=0, SysMain service disabled");
    PrintAuditRow("Отключить дефрагментацию системных файлов", ssd.disableBootDefrag, "BootOptimizeFunction Enable=N");
    PrintAuditRow("Отключить запись времени доступа к файлу", ssd.disableLastAccess, "NtfsDisableLastAccessUpdate=1");
    PrintAuditRow("Включить поддержку функции TRIM", ssd.enableTrimSupport, "NtfsDisableDeleteNotification=0 (TRIM Active)");
    PrintAuditRow("Отключить проверку нехватки места", ssd.disableLowDiskSpaceChecks, "Policies Explorer NoLowDiskSpaceChecks=1");
    PrintAuditRow("Оставлять ядро системы в памяти (RAM)", ssd.disablePagingExecutive, "Memory Management DisablePagingExecutive=1");
    PrintAuditRow("Большой кэш файловой системы", ssd.largeSystemCache, "Memory Management LargeSystemCache=1");
    PrintAuditRow("Приоритет квантов CPU играм (0x26)", ssd.win32PrioritySeparation, "PriorityControl Win32PrioritySeparation=38 (0x26)");

    PrintLn(ColYellow, "\n=== [ 4. TELEMETRY & PRIVACY SUB-TWEAKS AUDIT (15 Items) ] =======================");
    SystemTweaker::Get().DetectTelemetrySubState();
    const auto& tel = SystemTweaker::Get().GetTelemetrySubTweaks();

    PrintAuditRow("Отключить рекламный идентификатор", tel.disableAdvertisingId, "AdvertisingInfo DisabledByGroupPolicy=1");
    PrintAuditRow("Отключить синхронизацию Windows", tel.disableSettingSync, "SettingSync DisableSettingSync=2");
    PrintAuditRow("Отключить телеметрию Windows и VS", tel.disableVsTelemetry, "DataCollection AllowTelemetry=0, SQM OptIn=0");
    PrintAuditRow("Отключить телеметрию MS Office", tel.disableOfficeTelemetry, "office common privacy disconnectedstate=2");
    PrintAuditRow("Отключить телеметрию NVIDIA", tel.disableNvidiaTelemetry, "NvTelemetryContainer stopped & tasks disabled");
    PrintAuditRow("Отключить задания CEIP планировщика", tel.disableCeipTasks, "Consolidator, UsbCeip, KernelCeipTask disabled");
    PrintAuditRow("Отключить инвентаризацию программ", tel.disableAppInventory, "CommercialDataOptIn=0, ProgramDataUpdater disabled");
    PrintAuditRow("Отключить сбор статистики использования", tel.disableAppUsageStats, "CloudContent DisableTailoredExperiences=1");
    PrintAuditRow("Отключить сбор рукописного ввода", tel.disableInkingTypingData, "InputPersonalization RestrictImplicitInk=1");
    PrintAuditRow("Отключить запись поведения (Timeline)", tel.disableUserBehavior, "Policies System PublishUserActivities=0");
    PrintAuditRow("Отключить геолокацию и датчики", tel.disableLocation, "LocationAndSensors DisableLocation=1");
    PrintAuditRow("Отключить запросы Feedback Hub", tel.disableFeedbackFrequency, "Siuf Rules NumberOfSIUFInPeriod=0");
    PrintAuditRow("Отключить фоновое обновление речи", tel.disableSpeechModelUpdate, "Policies Speech AllowSpeechModelUpdate=0");
    PrintAuditRow("Отключить эксперименты Microsoft", tel.disableExperiments, "DataCollection AllowExperimentation=0");
    PrintAuditRow("Отключить автологгер DiagTrack", tel.disableAutoLoggerDiagTrack, "Autologger AutoLogger-Diagtrack-Listener Start=0");

    PrintLn(ColYellow, "\n=== [ 5. SYSTEM SERVICES STATUS AUDIT ] ==========================================");
    SystemTweaker::Get().RefreshServiceStates();
    const auto& svcs = SystemTweaker::Get().GetServices();
    int disCount = 0;
    for (const auto& s : svcs) {
        if (s.disabled) disCount++;
        std::string tag = s.disabled ? "[STOPPED/DISABLED]" : "[RUNNING/ENABLED] ";
        ConsoleCol c = s.disabled ? ColGreen : ColWhite;
        Print(c, "  " + tag + " ");
        Print(ColWhite, s.displayName);
        PrintLn(ColDim, " (" + s.id + ")");
    }
    PrintLn(ColCyan, "\nОтключено служб: " + std::to_string(disCount) + " из " + std::to_string(svcs.size()));
}

void TestMemoryCleaner() {
    PrintLn(ColCyan, "\n--- [ ТЕСТИРОВАНИЕ MEMORY CLEANER (Низкоуровневый сброс ОЗУ) ] ---");
    DetailedMemoryStats before = MemoryCleaner::Get().QueryDetailedStats();
    PrintLn(ColWhite, "Состояние ДО очистки:");
    PrintLn(ColWhite, "  Использовано ОЗУ: " + std::to_string(before.usedRAM_GB) + " GB (" + std::to_string((int)before.usedPercent) + "%)");
    PrintLn(ColWhite, "  Кэш Standby:      " + std::to_string(before.standbyRAM_GB) + " GB");

    PrintLn(ColYellow, "\nВыполняю NtSetSystemInformation (SystemPurgeStandbyList) и EmptyWorkingSet...");
    size_t freedBytes = 0;
    std::string status;
    bool ok = MemoryCleaner::Get().CleanAll(freedBytes, status);

    DetailedMemoryStats after = MemoryCleaner::Get().QueryDetailedStats();
    PrintLn(ColGreen, "Результат: " + status);
    PrintLn(ColWhite, "Состояние ПОСЛЕ очистки:");
    PrintLn(ColWhite, "  Использовано ОЗУ: " + std::to_string(after.usedRAM_GB) + " GB (" + std::to_string((int)after.usedPercent) + "%)");
    PrintLn(ColWhite, "  Кэш Standby:      " + std::to_string(after.standbyRAM_GB) + " GB");
    double freedMB = (double)freedBytes / (1024.0 * 1024.0);
    PrintLn(ColGreen, "  РЕАЛЬНО ОСВОБОЖДЕНО: " + std::to_string((int)freedMB) + " MB\n");
}

void TestTimerResolution() {
    PrintLn(ColCyan, "\n--- [ ТЕСТИРОВАНИЕ СИСТЕМНОГО ТАЙМЕРА (Timer Resolution) ] ---");
    float beforeMs = SystemInfo::Get().QueryLiveTimerResolution();
    PrintLn(ColWhite, "Текущее системное разрешение ДО включения: " + std::to_string(beforeMs) + " ms");

    PrintLn(ColYellow, "Запрашиваю 0.500 ms через NtSetTimerResolution(5000, TRUE)...");
    TimerResolutionService::Get().Start(0.500f);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    float afterMs = SystemInfo::Get().QueryLiveTimerResolution();
    PrintLn(ColGreen, "Фактическое системное разрешение ПОСЛЕ применения: " + std::to_string(afterMs) + " ms");
    if (afterMs <= 0.510f) {
        PrintLn(ColGreen, "✓ Таймер УСПЕШНО синхронизирован на максимальную точность 0.500 ms!\n");
    } else {
        PrintLn(ColYellow, "! Таймер ограничен операционной системой на уровне: " + std::to_string(afterMs) + " ms\n");
    }
}

void RunLiveMonitor() {
    PrintLn(ColCyan, "\n==================================================================================");
    PrintLn(ColWhite, "   РЕЖИМ МОНИТОРИНГА В РЕАЛЬНОМ ВРЕМЕНИ (Нажмите любую клавишу для выхода)");
    PrintLn(ColCyan, "==================================================================================");

    while (!_kbhit()) {
        DetailedMemoryStats mem = MemoryCleaner::Get().QueryDetailedStats();
        float timerMs = SystemInfo::Get().QueryLiveTimerResolution();
        SystemInfo::Get().Refresh();
        const auto& sp = SystemInfo::Get().GetSpecs();

        std::ostringstream ss;
        ss << "\r[RAM: " << std::fixed << std::setprecision(2) << mem.usedRAM_GB << " / " << mem.totalRAM_GB 
           << " GB (" << (int)mem.usedPercent << "%) | Standby Cache: " << mem.standbyRAM_GB 
           << " GB]  [Timer: " << std::setprecision(3) << timerMs << " ms]  [Plan: " 
           << sp.activePowerPlan.substr(0, 22) << "...]   ";

        Print(ColGreen, ss.str());
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    _getch(); // Clear keypress
    PrintLn(ColReset, "\nМониторинг остановлен.\n");
}

} // namespace

int RunDebugConsoleEngine() {
    InitConsole();
    PrintHeader();
    PrintSpecsDashboard();

    // Check command line arguments for non-interactive execution
    std::wstring cmdLine = GetCommandLineW();
    bool nonInteractive = (cmdLine.find(L"--audit") != std::wstring::npos ||
                           cmdLine.find(L"--test-all") != std::wstring::npos ||
                           cmdLine.find(L"--non-interactive") != std::wstring::npos);

    if (nonInteractive) {
        PrintLn(ColYellow, "[CLI MODE] Запуск автоматического системного аудита...");
        RunLiveTweakAudit();
        PrintLn(ColGreen, "\n[CLI MODE] Аудит завершен успешно.");
        return 0;
    }

    // Default interactive loop
    RunLiveTweakAudit();

    bool running = true;
    while (running) {
        PrintLn(ColCyan, "\n==================================================================================");
        PrintLn(ColWhite, "   ВЫБЕРИТЕ ДЕЙСТВИЕ ДЛЯ ТЕСТИРОВАНИЯ И ДИАГНОСТИКИ:");
        PrintLn(ColCyan, "==================================================================================");
        PrintLn(ColYellow, "  [1] Повторить полный аудит системы (Live Audit)");
        PrintLn(ColWhite,  "  [2] Применить Esports Latency Tweaks (Timer, MMCSS, BCD, TCP)");
        PrintLn(ColWhite,  "  [3] Применить OS & SSD Performance Tweaks (12+8 твиков)");
        PrintLn(ColWhite,  "  [4] Применить Telemetry & Privacy Tweaks (15 твиков)");
        PrintLn(ColGreen,  "  [5] Запустить тест Memory Cleaner (Сброс Working Sets + Standby Cache)");
        PrintLn(ColGreen,  "  [6] Запустить тест Timer Resolution (Установка 0.500 ms + проверка)");
        PrintLn(ColCyan,   "  [7] Режим мониторинга в реальном времени (Live Monitor)");
        PrintLn(ColRed,    "  [8] Откатить все твики к значениям по умолчанию Windows (Revert)");
        PrintLn(ColDim,    "  [0] Выход из программы");
        Print(ColYellow,   "\nВведите номер команды > ");

        std::string choice;
        if (!std::getline(std::cin, choice)) {
            break;
        }
        // Strip UTF-8 BOM if present (0xEF, 0xBB, 0xBF)
        if (choice.size() >= 3 && (unsigned char)choice[0] == 0xEF && (unsigned char)choice[1] == 0xBB && (unsigned char)choice[2] == 0xBF) {
            choice.erase(0, 3);
        }
        // Strip whitespace and control characters
        while (!choice.empty() && (unsigned char)choice.front() <= 32) {
            choice.erase(choice.begin());
        }
        while (!choice.empty() && (unsigned char)choice.back() <= 32) {
            choice.pop_back();
        }
        if (choice.empty()) {
            continue;
        }

        if (choice == "1") {
            PrintSpecsDashboard();
            RunLiveTweakAudit();
        } else if (choice == "2") {
            PrintLn(ColCyan, "Применяю киберспортивные твики задержки...");
            std::string status;
            TweaksEngine::Get().ApplyPreset(PresetType::EsportsUltra, status);
            PrintLn(ColGreen, "Статус: " + status);
        } else if (choice == "3") {
            PrintLn(ColCyan, "Применяю 12 твиков ОС и 8 твиков SSD...");
            std::string msg;
            SystemTweaker::Get().ApplyAllOsPerfSubTweaks(true, msg);
            PrintLn(ColGreen, "ОС: " + msg);
            SystemTweaker::Get().ApplyAllSsdPerfSubTweaks(true, msg);
            PrintLn(ColGreen, "SSD: " + msg);
        } else if (choice == "4") {
            PrintLn(ColCyan, "Блокирую 15 телеметрий Windows и Office...");
            std::string msg;
            SystemTweaker::Get().ApplyAllTelemetrySubTweaks(true, msg);
            PrintLn(ColGreen, "Телеметрия: " + msg);
        } else if (choice == "5") {
            TestMemoryCleaner();
        } else if (choice == "6") {
            TestTimerResolution();
        } else if (choice == "7") {
            RunLiveMonitor();
        } else if (choice == "8") {
            PrintLn(ColYellow, "Откатываю все параметры к значениям Windows...");
            std::string status;
            TweaksEngine::Get().RevertToDefaults(status);
            PrintLn(ColWhite, "TweaksEngine: " + status);
            SystemTweaker::Get().RevertAllTweaks(status);
            PrintLn(ColWhite, "SystemTweaker: " + status);
            PrintLn(ColGreen, "Все твики успешно сброшены к значениям по умолчанию.");
        } else if (choice == "0" || choice == "q" || choice == "Q") {
            running = false;
        } else {
            PrintLn(ColRed, "Неизвестная команда [" + choice + "]. Введите число от 0 до 8.");
        }
    }

    PrintLn(ColCyan, "Завершение работы консоли диагностики. До свидания!");
    return 0;
}

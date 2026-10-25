#include "system_tweaker.h"
#include "native_bridge.h"
#include "common_utils.h"
#include "memory_cleaner.h"
#include "debug_logger.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <future>
#include <vector>
#include <atomic>

#include <powrprof.h>
#pragma comment(lib, "powrprof.lib")

namespace fs = std::filesystem;

// Win32 Power Management Precision Setting GUIDs
static const GUID WCS_SUB_VIDEO = { 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };
static const GUID WCS_VIDEOIDLE = { 0x3c0bc021, 0xc8a8, 0x4e07, { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } };

static const GUID WCS_SUB_DISK = { 0x0012ee47, 0x9041, 0x4b5d, { 0x9b, 0x77, 0x53, 0x5f, 0xba, 0x8b, 0x14, 0x42 } };
static const GUID WCS_DISKIDLE = { 0x6738e2c4, 0xe8a5, 0x4a42, { 0xb1, 0x6a, 0xe0, 0x40, 0xe7, 0x69, 0x75, 0x6e } };

static const GUID WCS_SUB_PROC = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID WCS_PERFBOOST = { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };

static const GUID WCS_SUB_SLEEP = { 0x238c9fa8, 0x0aad, 0x41ed, { 0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20 } };
static const GUID WCS_STANDBYIDLE = { 0x29f6c1db, 0x86da, 0x48c5, { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } };

static const GUID WCS_GUID_BALANCED = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
static const GUID WCS_GUID_HIGH_PERF = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };

static bool SetActivePowerSchemeToUltimate() {
    GUID schemeGuid = GUID_NULL;
    DWORD index = 0;
    DWORD size = sizeof(GUID);
    bool found = false;

    while (PowerEnumerate(NULL, NULL, NULL, ACCESS_SCHEME, index++, (UCHAR*)&schemeGuid, &size) == ERROR_SUCCESS) {
        UCHAR nameBuf[512] = { 0 };
        DWORD nameSize = sizeof(nameBuf);
        if (PowerReadFriendlyName(NULL, &schemeGuid, NULL, NULL, nameBuf, &nameSize) == ERROR_SUCCESS) {
            std::wstring wname = reinterpret_cast<wchar_t*>(nameBuf);
            if (wname.find(L"Ultimate") != std::wstring::npos ||
                wname.find(L"Максимальная") != std::wstring::npos ||
                wname.find(L"Esports") != std::wstring::npos) {
                found = true;
                break;
            }
        }
        size = sizeof(GUID);
    }

    if (found) {
        PowerSetActiveScheme(NULL, &schemeGuid);
        return true;
    }

    // Duplicate once if not found
    SystemTweaker::RunCmdSilent(L"powercfg -duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61");
    index = 0;
    size = sizeof(GUID);
    while (PowerEnumerate(NULL, NULL, NULL, ACCESS_SCHEME, index++, (UCHAR*)&schemeGuid, &size) == ERROR_SUCCESS) {
        UCHAR nameBuf[512] = { 0 };
        DWORD nameSize = sizeof(nameBuf);
        if (PowerReadFriendlyName(NULL, &schemeGuid, NULL, NULL, nameBuf, &nameSize) == ERROR_SUCCESS) {
            std::wstring wname = reinterpret_cast<wchar_t*>(nameBuf);
            if (wname.find(L"Ultimate") != std::wstring::npos ||
                wname.find(L"Максимальная") != std::wstring::npos) {
                PowerSetActiveScheme(NULL, &schemeGuid);
                return true;
            }
        }
        size = sizeof(GUID);
    }

    PowerSetActiveScheme(NULL, &WCS_GUID_HIGH_PERF);
    return true;
}

static bool SetActivePowerSchemeToBalanced() {
    PowerSetActiveScheme(NULL, &WCS_GUID_BALANCED);
    return true;
}

SystemTweaker& SystemTweaker::Get() {
    static SystemTweaker instance;
    return instance;
}

SystemTweaker::SystemTweaker() {
    InitServiceDatabase();
    InitAppxDatabase();
    DetectFastState();
}

void SystemTweaker::DetectFastState() {
    std::lock_guard<std::mutex> lock(m_configMutex);
    DetectSystemState();
    DetectSecurityState();
    DetectOsPerfSubState();
    DetectSsdPerfSubState();
    DetectTelemetrySubState();
}

void SystemTweaker::Init() {
    DetectFastState();
    InitAsync();
}

void SystemTweaker::InitAsync(std::function<void()> onComplete) {
    std::thread([this, onComplete]() {
        // 1. SCM 52 Services live enumeration in worker thread
        RefreshServiceStates();
        m_servicesScanned.store(true);

        // 2. SCM telemetry service query
        DetectTelemetrySubState();

        // 3. SSD parameter checks via direct registry (instant, 0ms)
        DWORD lastAccess = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisableLastAccessUpdate", 0);
        DWORD dot3 = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisable8dot3NameCreation", 0);
        DWORD trimVal = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisableDeleteNotification", 0);
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_ssdPerfSub.disableLastAccess = (lastAccess >= 1 || dot3 >= 1);
            m_ssdPerfSub.enableTrimSupport = (trimVal == 0);
        }
        m_ssdScanned.store(true);

        // 4. Instant AppX packages scan via registry in worker thread
        ScanInstalledAppxPackages();
        m_appxScanned.store(true);
        NativeBridge::Get().BroadcastEvent("appx_scanned", NativeBridge::Get().HandleGetAppx());

        m_isInitialized.store(true);
        if (onComplete) {
            onComplete();
        }
    }).detach();
}

// -----------------------------------------------------------------------------
// Registry & Command Helpers
// -----------------------------------------------------------------------------
bool SystemTweaker::RegSetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD val) {
    bool ok = RegUtil::SetDword(hRoot, subKey, valName, val);
    std::wstring rootName = (hRoot == HKEY_LOCAL_MACHINE) ? L"HKLM" : ((hRoot == HKEY_CURRENT_USER) ? L"HKCU" : L"HKCR");
    DebugLogger::Get().LogRegistry(rootName, subKey, valName, val, ok);
    return ok;
}

bool SystemTweaker::RegSetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, const std::wstring& val) {
    return RegUtil::SetString(hRoot, subKey, valName, val);
}

bool SystemTweaker::RegDeleteVal(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName) {
    return RegUtil::DeleteVal(hRoot, subKey, valName);
}

DWORD SystemTweaker::RegGetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD defaultVal) {
    DWORD val = 0;
    if (RegUtil::GetDword(hRoot, subKey, valName, val)) {
        return val;
    }
    return defaultVal;
}

std::wstring SystemTweaker::RegGetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName) {
    std::wstring val;
    RegUtil::GetString(hRoot, subKey, valName, val);
    return val;
}

bool SystemTweaker::RunCmdSilent(const std::wstring& cmd, DWORD timeoutMs) {
    const std::string sCmd = WideStrToUtf8(cmd);
    DEBUG_LOG_INFO("ExecCmd", sCmd.c_str());

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring cmdLine;
    if (cmd.find(L"|") != std::wstring::npos || cmd.find(L">") != std::wstring::npos || cmd.find(L"<") != std::wstring::npos || cmd.find(L"&") != std::wstring::npos) {
        cmdLine = L"cmd.exe /c " + cmd;
    } else {
        cmdLine = cmd;
    }
    std::vector<wchar_t> modifiableCmd(cmdLine.begin(), cmdLine.end());
    modifiableCmd.push_back(L'\0');

    if (CreateProcessW(nullptr, modifiableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ScopedHandle hProc(pi.hProcess);
        ScopedHandle hThrd(pi.hThread);
        DWORD waitRes = WaitForSingleObject(hProc, timeoutMs);
        if (waitRes == WAIT_TIMEOUT) {
            TerminateProcess(hProc, 1);
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(hProc, &exitCode);
        return (exitCode == 0);
    }
    DEBUG_LOG_ERROR("ExecCmd", std::string("Failed to spawn: ") + sCmd);
    return false;
}

bool SystemTweaker::RunCmdWithOutput(const std::wstring& cmd, std::string& outOutput, DWORD timeoutMs) {
    outOutput.clear();
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    ScopedHandle hReadPipe, hWritePipe;
    if (!CreatePipe(hReadPipe.put(), hWritePipe.put(), &sa, 0)) {
        return false;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi = { 0 };
    std::wstring cmdLine;
    if (cmd.find(L"|") != std::wstring::npos || cmd.find(L">") != std::wstring::npos || cmd.find(L"<") != std::wstring::npos || cmd.find(L"&") != std::wstring::npos) {
        cmdLine = L"cmd.exe /c " + cmd;
    } else {
        cmdLine = cmd;
    }
    std::vector<wchar_t> modifiableCmd(cmdLine.begin(), cmdLine.end());
    modifiableCmd.push_back(L'\0');

    if (CreateProcessW(nullptr, modifiableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ScopedHandle hProc(pi.hProcess);
        ScopedHandle hThrd(pi.hThread);
        hWritePipe.Close();

        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            outOutput += buffer;
        }

        WaitForSingleObject(hProc, timeoutMs);
        DWORD exitCode = 0;
        GetExitCodeProcess(hProc, &exitCode);
        return (exitCode == 0);
    }

    return false;
}

uint64_t SystemTweaker::DeleteDirectoryFiles(const std::wstring& dirPath, bool recursive) {
    if (dirPath.empty()) return 0;

    DWORD rootAttrs = GetFileAttributesW(dirPath.c_str());
    if (rootAttrs == INVALID_FILE_ATTRIBUTES) return 0;

    // If it's a single file, delete it directly
    if (!(rootAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        uint64_t fSize = 0;
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(dirPath.c_str(), GetFileExInfoStandard, &fad)) {
            fSize = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
        }
        if (rootAttrs & FILE_ATTRIBUTE_READONLY) {
            SetFileAttributesW(dirPath.c_str(), rootAttrs & ~FILE_ATTRIBUTE_READONLY);
        }
        if (DeleteFileW(dirPath.c_str())) {
            return fSize;
        }
        return 0;
    }

    // CRITICAL: NEVER recurse into the root folder if it is a reparse point / junction!
    if (rootAttrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        return 0;
    }

    uint64_t freedBytes = 0;
    std::vector<std::wstring> dirStack;
    std::vector<std::wstring> emptyDirsToRemove;
    dirStack.push_back(dirPath);
    auto startTime = std::chrono::steady_clock::now();

    while (!dirStack.empty()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() > 1500) {
            break; // Hard safety cap: max 1.5s per root directory branch
        }
        std::wstring currentDir = std::move(dirStack.back());
        dirStack.pop_back();

        while (!currentDir.empty() && (currentDir.back() == L'\\' || currentDir.back() == L'/')) {
            currentDir.pop_back();
        }

        std::wstring searchPattern = currentDir + L"\\*";
        WIN32_FIND_DATAW ffd;
        HANDLE hFind = FindFirstFileExW(searchPattern.c_str(), FindExInfoBasic, &ffd,
                                         FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (ffd.cFileName[0] == L'.') {
                if (ffd.cFileName[1] == L'\0' || (ffd.cFileName[1] == L'.' && ffd.cFileName[2] == L'\0')) {
                    continue;
                }
            }

            std::wstring fullPath = currentDir + L"\\" + ffd.cFileName;

            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // CRITICAL: Check FILE_ATTRIBUTE_REPARSE_POINT!
                // NEVER recurse into directory junctions, symlinks, or reparse points!
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    continue;
                }
                if (recursive) {
                    dirStack.push_back(fullPath);
                    emptyDirsToRemove.push_back(fullPath);
                }
            } else {
                // Regular file
                uint64_t fileSize = ((uint64_t)ffd.nFileSizeHigh << 32) | (uint64_t)ffd.nFileSizeLow;
                if (ffd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesW(fullPath.c_str(), ffd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                // Skip locked or in-use files immediately without hanging
                if (DeleteFileW(fullPath.c_str())) {
                    freedBytes += fileSize;
                }
            }
        } while (FindNextFileW(hFind, &ffd));

        FindClose(hFind);
    }

    // Try removing empty subdirectories from deepest level up
    for (auto it = emptyDirsToRemove.rbegin(); it != emptyDirsToRemove.rend(); ++it) {
        RemoveDirectoryW(it->c_str());
    }

    return freedBytes;
}

uint64_t SystemTweaker::DeleteDirectoriesParallel(const std::vector<std::wstring>& paths, bool recursive) {
    if (paths.empty()) return 0;
    if (paths.size() == 1) return DeleteDirectoryFiles(paths[0], recursive);

    std::vector<std::future<uint64_t>> futures;
    futures.reserve(paths.size());
    for (const auto& p : paths) {
        futures.push_back(std::async(std::launch::async, [p, recursive]() {
            return DeleteDirectoryFiles(p, recursive);
        }));
    }

    uint64_t total = 0;
    for (auto& fut : futures) {
        try {
            total += fut.get();
        } catch (...) {}
    }
    return total;
}

// -----------------------------------------------------------------------------
// Database Initialization
// -----------------------------------------------------------------------------
void SystemTweaker::InitServiceDatabase() {
    m_services.clear();

    // Helper lambda to register service items
    auto addSvc = [&](const std::string& id, const std::string& name, const std::string& desc,
                      const std::vector<std::wstring>& winNames, DWORD defStart, bool recDisable, bool danger = false) {
        ServiceItemInfo item;
        item.id = id;
        item.displayName = name;
        item.description = desc;
        item.winServiceNames = winNames;
        item.defaultStartType = defStart;
        item.recommendedDisable = recDisable;
        item.isDanger = danger;
        item.disabled = false;
        m_services.push_back(item);
    };

    // Services corresponding to official Windows Russian localization
    addSvc("svc_search", "Windows Search (Индексация поиска)", "Индексирование содержимого файлов, кэширование свойств и результатов системного поиска", { L"WSearch" }, SERVICE_AUTO_START, true, true);
    addSvc("svc_time", "Служба времени Windows (W32Time)", "Поддерживает синхронизацию даты и времени на клиентах и серверах в сети", { L"W32Time" }, SERVICE_DEMAND_START, false);
    addSvc("svc_radio", "Служба управления радиомодулями (RmSvc)", "Управление радиосвязью, режимом «В самолете», Bluetooth и датчиками связи", { L"RmSvc" }, SERVICE_DEMAND_START, false);
    addSvc("svc_superfetch", "SysMain (Кэширование памяти)", "Анализирует запуск программ и кэширует часто используемые файлы в оперативную память", { L"SysMain" }, SERVICE_AUTO_START, true);
    addSvc("svc_scanner", "Служба загрузки изображений Windows (WIA)", "Обеспечивает взаимодействие со сканерами, цифровыми камерами и устройствами захвата изображений", { L"stisvc" }, SERVICE_DEMAND_START, false);
    addSvc("svc_geo", "Служба геолокации Windows (lfsvc)", "Отслеживает текущее географическое положение системы и передает геопозицию приложениям", { L"lfsvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_ajrouter", "Маршрутизатор AllJoyn (AJRouter)", "Маршрутизирует сообщения локальной сети для платформы умного дома и интернета вещей AllJoyn", { L"AJRouter" }, SERVICE_DEMAND_START, true);
    addSvc("svc_wpn_user", "Служба push-уведомлений Windows (WpnUserService)", "Обеспечивает прием и отображение push-уведомлений для приложений текущего пользователя (требуется для NVIDIA App, Discord и др.)", { L"WpnUserService" }, SERVICE_AUTO_START, false, true);
    addSvc("svc_sensors", "Служба датчиков ориентации и освещенности (SensorService)", "Управляет встроенными датчиками освещенности, гироскопами и автоматическим поворотом экрана", { L"SensorService", L"SensrSvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_termsrv", "Службы удаленных рабочих столов (TermService)", "Разрешает пользователям удаленно подключаться к этому компьютеру через протокол RDP", { L"TermService" }, SERVICE_DEMAND_START, false);
    addSvc("svc_spooler", "Диспетчер очереди печати (Spooler)", "Управляет очередью печати документов и передачей заданий на локальные и сетевые принтеры", { L"Spooler" }, SERVICE_AUTO_START, false);
    addSvc("svc_wuauserv", "Центр обновления Windows (wuauserv)", "Автоматический поиск, фоновое скачивание и установка системных обновлений и исправлений безопасности", { L"wuauserv" }, SERVICE_DEMAND_START, true, true);
    addSvc("svc_xbox", "Сетевые службы Xbox Live", "Обеспечивает авторизацию в учетной записи Xbox Live, сетевую игру и облачные сохранения", { L"XblAuthManager", L"XblGameSave", L"XboxNetApiSvc", L"XboxGipSvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_bth", "Служба поддержки Bluetooth (bthserv)", "Обнаружение, сопряжение и согласование работы беспроводных устройств Bluetooth (наушники, геймпады)", { L"bthserv" }, SERVICE_DEMAND_START, false);
    addSvc("svc_themes", "Темы оформления Windows (Themes)", "Управление визуальными темами, стилями оформления окон, обоями и графическими эффектами Aero", { L"Themes" }, SERVICE_AUTO_START, false);
    addSvc("svc_tablets", "Сенсорная клавиатура и рукописный ввод", "Обеспечивает работу экранной сенсорной клавиатуры и распознавание рукописного ввода пера", { L"TabletInputService" }, SERVICE_DEMAND_START, true);
    addSvc("svc_biometric", "Биометрическая служба Windows Hello (WbioSrvc)", "Сбор, защита и обработка биометрических данных для авторизации по отпечатку пальца или лицу", { L"WbioSrvc" }, SERVICE_DEMAND_START, false);
    addSvc("svc_iphlpsvc", "Вспомогательная служба IP (IPv6 / IP Helper)", "Обеспечивает поддержку туннелирования IPv6 (6to4, ISATAP, Teredo) и расширенных сетевых протоколов", { L"iphlpsvc" }, SERVICE_AUTO_START, true);
    addSvc("svc_userdata", "Служба данных пользователя (UserDataSvc)", "Управляет доступом системных приложений к локальным контактам, календарям и персональным данным", { L"UserDataSvc", L"UnistoreSvc" }, SERVICE_DEMAND_START, false);
    addSvc("svc_wscsvc", "Центр обеспечения безопасности Windows (wscsvc)", "Отслеживает и сообщает параметры работоспособности антивируса, брандмауэра и безопасности системы", { L"wscsvc" }, SERVICE_AUTO_START, true);
    addSvc("svc_diagtrack", "Служба сбора диагностических данных и телеметрии (DiagTrack)", "Собирает диагностическую информацию о работе Windows, сбоях и передает телеметрию в Microsoft", { L"DiagTrack", L"dmwappushservice" }, SERVICE_AUTO_START, true);
    addSvc("svc_pcasvc", "Помощник по совместимости программ (PcaSvc)", "Отслеживает ошибки запуска старых программ и автоматически применяет параметры совместимости", { L"PcaSvc" }, SERVICE_AUTO_START, true);
    addSvc("svc_defrag", "Оптимизация и дефрагментация дисков (defragsvc)", "Оптимизирует файловую систему жестких дисков (дефрагментация) и твердотельных накопителей (TRIM)", { L"defragsvc" }, SERVICE_DEMAND_START, false);
    addSvc("svc_dosvc", "Оптимизация доставки обновлений (Delivery Optimization)", "Пиринговая (P2P) передача и скачивание обновлений Windows и приложений Microsoft Store", { L"DoSvc" }, SERVICE_AUTO_START, true);
    addSvc("svc_pimindex", "Служба индексации контактов (PimIndexMaintenanceSvc)", "Индексирует контактные данные пользователей для быстрого локального поиска", { L"PimIndexMaintenanceSvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_winstore", "Служба установки Microsoft Store и пакетов AppX", "Обеспечивает фоновую инфраструктуру установки, обновления и развертывания приложений магазина Store", { L"InstallService", L"AppXSvc" }, SERVICE_DEMAND_START, false, true);
    addSvc("svc_remotereg", "Дистанционное управление реестром (RemoteRegistry)", "Позволяет удаленным пользователям просматривать и изменять параметры реестра на этом компьютере", { L"RemoteRegistry" }, SERVICE_DEMAND_START, true);
    addSvc("svc_onesync", "Служба синхронизации почты и контактов (OneSyncSvc)", "Синхронизирует почту, календарь, контакты и данные учетных записей Microsoft", { L"OneSyncSvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_dssvc", "Служба брокера обмена данными (DsSvc)", "Обеспечивает защищенный обмен данными и взаимодействие между изолированными UWP-приложениями", { L"DsSvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_devicesflow", "Служба маршрутизации устройств (DevicesFlowUserSvc)", "Управляет потоками мультимедийных данных при трансляции на внешние сетевые устройства", { L"DevicesFlowUserSvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_vmic", "Службы интеграции Hyper-V", "Обеспечивает взаимодействие, обмен временем и выключение гостевых виртуальных машин Hyper-V", { L"vmickvpexchange", L"vmicguestinterface", L"vmicshutdown", L"vmicheartbeat" }, SERVICE_DEMAND_START, true, true);
    addSvc("svc_maps", "Диспетчер скачанных карт (MapsBroker)", "Предоставляет доступ сторонним приложениям к загруженным офлайн-картам Windows", { L"MapsBroker" }, SERVICE_DEMAND_START, true);
    addSvc("svc_wpnservice", "Системные push-уведомления (WpnService)", "Системная служба маршрутизации push-уведомлений и оповещений приложений", { L"WpnService" }, SERVICE_AUTO_START, false, true);
    addSvc("svc_bcastdvr", "Служба фоновой записи игр и трансляций (BcastDVR)", "Фоновая запись видео игрового процесса (Game DVR), захват клипов и прямые трансляции", { L"BcastDVRUserService" }, SERVICE_DEMAND_START, true);
    addSvc("svc_cbdhsvc", "Служба облачного буфера обмена (cbdhsvc)", "Синхронизирует историю буфера обмена (Win+V) между устройствами через облако Microsoft", { L"cbdhsvc" }, SERVICE_DEMAND_START, false);
    addSvc("svc_wlidsvc", "Помощник по входу в учетную запись Microsoft (wlidsvc)", "Обеспечивает проверку подлинности и вход в систему с использованием учетной записи Microsoft", { L"wlidsvc" }, SERVICE_DEMAND_START, false);
    addSvc("svc_ikeext", "Модули ключей IPsec для IKE и AuthIP (IKEEXT)", "Управляет ключами шифрования и проверкой подлинности для безопасных сетевых соединений IPsec и VPN", { L"IKEEXT" }, SERVICE_DEMAND_START, true);
    addSvc("svc_fax", "Служба факсов и телефонии (Fax)", "Управляет отправкой, приемом и печатью факсов, а также модемным телефонным набором", { L"Fax", L"TapiSrv" }, SERVICE_DEMAND_START, true);
    addSvc("svc_dps", "Служба диагностической политики (DPS)", "Обнаруживает неполадки в работе компонентов Windows и запускает средства устранения проблем", { L"DPS", L"WdiServiceHost", L"WdiSystemHost" }, SERVICE_AUTO_START, true, true);
    addSvc("svc_wersvc", "Служба регистрации ошибок Windows (WerSvc)", "Формирует отчеты о падениях программ и системных ошибках для отправки в службу поддержки Microsoft", { L"WerSvc" }, SERVICE_DEMAND_START, true, true);
    addSvc("svc_webclient", "Служба веб-клиента WebDAV (WebClient)", "Позволяет программам Windows открывать, редактировать и сохранять файлы на серверах WebDAV", { L"WebClient" }, SERVICE_DEMAND_START, true);
    addSvc("svc_wmiap", "Адаптер производительности WMI (wmiApSrv)", "Предоставляет данные системных счетчиков производительности через библиотеки WMI", { L"wmiApSrv" }, SERVICE_DEMAND_START, true);
    addSvc("svc_wwansvc", "Служба автонастройки беспроводных сетей WWAN (модемы)", "Управляет встроенными и USB мобильными модемами сотовых сетей 3G/4G/5G и SIM-картами", { L"WwanSvc" }, SERVICE_DEMAND_START, true);
    addSvc("svc_wifidirect", "Служба поддержки беспроводных дисплеев (Wi-Fi Direct)", "Обеспечивает подключение беспроводных мониторов, Miracast и прямое соединение по Wi-Fi Direct", { L"WiFiDirectServicesSupportService" }, SERVICE_DEMAND_START, true);
    addSvc("svc_scard", "Служба смарт-карт (SCardSvr)", "Управляет доступом к физическим смарт-картам, криптографическим USB-токенам и аппаратным ключам", { L"SCardSvr" }, SERVICE_DEMAND_START, true);
    addSvc("svc_wallet", "Служба электронного кошелька (WalletService)", "Обеспечивает работу платежной системы Windows Wallet и бесконтактных NFC транзакций", { L"WalletService" }, SERVICE_DEMAND_START, true);
    addSvc("svc_retaildemo", "Служба демонстрации магазина (RetailDemo)", "Специальный режим витринной демонстрации возможностей Windows для торговых залов", { L"RetailDemo" }, SERVICE_DEMAND_START, true);
}

static void ScanAppxRegistryPackageNames(std::vector<std::string>& installedNames) {
    auto scanRegKey = [&](HKEY hRoot, const std::wstring& subPath) {
        ScopedHKey hKey;
        if (RegOpenKeyExW(hRoot, subPath.c_str(), 0, KEY_READ, hKey.put()) == ERROR_SUCCESS) {
            DWORD subCount = 0;
            RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &subCount, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            for (DWORD i = 0; i < subCount; ++i) {
                wchar_t nameBuf[512] = { 0 };
                DWORD nameLen = 511;
                if (RegEnumKeyExW(hKey, i, nameBuf, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    nameBuf[511] = L'\0';
                    std::string s;
                    s.reserve(nameLen);
                    for (DWORD c = 0; c < nameLen; ++c) {
                        s.push_back(static_cast<char>(::tolower(nameBuf[c])));
                    }
                    installedNames.push_back(s);
                }
            }
        }
    };

    scanRegKey(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages");
    scanRegKey(HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Families");
    scanRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\PackageRepository\\Packages");
    scanRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Applications");
    scanRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\InboxApplications");
    scanRegKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\ConfiguredApplications");
}

static bool CheckAppxPatternMatch(const std::string& pattern, const std::vector<std::string>& installedNames) {
    std::stringstream ss(pattern);
    std::string token;
    while (std::getline(ss, token, ';')) {
        std::stringstream ss2(token);
        std::string subToken;
        while (std::getline(ss2, subToken, ',')) {
            std::string clean;
            for (char c : subToken) {
                if (c != '*' && c != ' ' && c != '\t') {
                    clean.push_back(static_cast<char>(::tolower(c)));
                }
            }
            if (!clean.empty()) {
                for (const auto& instName : installedNames) {
                    if (instName.find(clean) != std::string::npos) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void SystemTweaker::InitAppxDatabase() {
    m_appxItems.clear();

    auto addApp = [&](const std::string& id, const std::string& name, const std::string& pattern, const std::string& cat) {
        AppxItemInfo item;
        item.id = id;
        item.displayName = name;
        item.packageNamePattern = pattern;
        item.category = cat;
        item.selected = false;
        item.isInstalled = false;
        m_appxItems.push_back(item);
    };

    // 1. Развлечения
    addApp("app_mr", "Mixed Reality", "*MixedReality.Portal*", "Развлечения");
    addApp("app_ymusic", "Яндекс Музыка", "*Yandex*Music*", "Развлечения");
    addApp("app_groove", "Музыка Groove", "*ZuneMusic*", "Развлечения");
    addApp("app_movies", "Кино и ТВ", "*ZuneVideo*", "Развлечения");
    addApp("app_weather", "Погода", "*BingWeather*", "Развлечения");
    addApp("app_photos", "Фото", "*Windows.Photos*", "Развлечения");
    addApp("app_solitaire", "Косынка", "*MicrosoftSolitaireCollection*", "Развлечения");
    addApp("app_camera", "Камера", "*WindowsCamera*", "Развлечения");
    addApp("app_alarms", "Часы", "*WindowsAlarms*", "Развлечения");
    addApp("app_xbox", "Xbox", "*XboxApp*;*GamingApp*;*XboxGamingOverlay*", "Развлечения");
    addApp("app_maps", "Карты", "*WindowsMaps*", "Развлечения");
    addApp("app_recorder", "Запись", "*WindowsSoundRecorder*", "Развлечения");
    addApp("app_stickynotes", "Записки", "*MicrosoftStickyNotes*", "Развлечения");

    // 2. Браузеры
    addApp("app_edgenew", "Edge New", "*MicrosoftEdge*", "Браузеры");
    addApp("app_edgeold", "Edge Old", "*MicrosoftEdge.Legacy*", "Браузеры");

    // 3. Кодеки
    addApp("app_heif", "HEIF", "*HEIFImageExtension*", "Кодеки");
    addApp("app_webmedia", "Web", "*WebMediaExtensions*", "Кодеки");
    addApp("app_webp", "WebP", "*WebpImageExtension*", "Кодеки");
    addApp("app_vp9", "VP9", "*VP9VideoExtensions*", "Кодеки");

    // 4. Антивирус & Безопасность
    addApp("app_defender", "Защитник", "*SecHealthUI*", "Антивирус");
    addApp("app_smartscreen", "SmartScreen", "*SmartScreen*", "Антивирус");
    addApp("app_security", "Безопасность", "*WindowsDefender*", "Антивирус");

    // 5. Средства установки UWP
    addApp("app_store", "Магазин", "*WindowsStore*", "Средства установки UWP");
    addApp("app_appinstaller", "Инсталлятор offline APPX", "*DesktopAppInstaller*", "Средства установки UWP");
    addApp("app_cdm", "Инсталлятор ПО реклам", "*ContentDeliveryManager*", "Средства установки UWP");
    addApp("app_tariffs", "Тарифы", "*PaidNetworkData*", "Средства установки UWP");
    addApp("app_wallet", "Кошелёк", "*Wallet*", "Средства установки UWP");

    // 6. Прикладное ПО
    addApp("app_office", "Office", "*MicrosoftOfficeHub*", "Прикладное ПО");
    addApp("app_onenote", "OneNote", "*Office.OneNote*", "Прикладное ПО");
    addApp("app_print3d", "Печать 3D", "*Print3D*", "Прикладное ПО");
    addApp("app_3dbuilder", "3D Builder", "*3DBuilder*", "Прикладное ПО");
    addApp("app_3dviewer", "3D Viewer", "*Microsoft3DViewer*", "Прикладное ПО");
    addApp("app_paint3d", "Paint 3D", "*MSPaint*", "Прикладное ПО");
    addApp("app_calc", "Калькулятор", "*WindowsCalculator*", "Прикладное ПО");
    addApp("app_barcode", "Bar-code", "*Barcode*", "Прикладное ПО");
    addApp("app_screensketch", "Наброски", "*ScreenSketch*", "Прикладное ПО");
    addApp("app_snipping", "Захват экрана", "*SnippingTool*", "Прикладное ПО");

    // 7. Коммуникации и связь
    addApp("app_cortana", "Cortana", "*549981C3F5F10*", "Коммуникации и связь");
    addApp("app_search", "Поиск Windows", "*WindowsSearch*", "Коммуникации и связь");
    addApp("app_onedrive", "OneDrive", "*OneDrive*", "Коммуникации и связь");
    addApp("app_mail", "Почта", "*windowscommunicationsapps*", "Коммуникации и связь");
    addApp("app_phone", "Телефон", "*YourPhone*", "Коммуникации и связь");
    addApp("app_people", "Люди", "*People*", "Коммуникации и связь");
    addApp("app_skype", "Skype", "*SkypeApp*", "Коммуникации и связь");
    addApp("app_messaging", "Сообщения", "*Messaging*", "Коммуникации и связь");
    addApp("app_feedback", "Отзывы", "*WindowsFeedbackHub*", "Коммуникации и связь");
    addApp("app_tips", "Советы", "*Getstarted*", "Коммуникации и связь");
    addApp("app_gethelp", "Техподдержка", "*GetHelp*", "Коммуникации и связь");

    // 8. Другие возможности
    addApp("app_eye", "Управление глазами", "*EyeControl*", "Другие возможности");
    addApp("app_narrator", "Быстрый запуск диктора", "*NarratorQuickStart*", "Другие возможности");
    addApp("app_kiosk", "Киоск", "*AssignedAccess*", "Другие возможности");
    addApp("app_family", "Родительский контроль", "*Family*", "Другие возможности");
    addApp("app_touch", "Сенсорный ввод", "*TouchKeyboard*", "Другие возможности");
    addApp("app_hello", "Windows Hello", "*BioEnrollment*", "Другие возможности");

    // 9. Интерфейсы и библиотеки UWP
    addApp("app_auth", "Форма авторизации", "*AccountsControl*", "Интерфейсы и библиотеки UWP");
    addApp("app_adv", "Посредник рекламы", "*Advertising*", "Интерфейсы и библиотеки UWP");
    addApp("app_lang", "Поддержка языков", "*LanguageFeatures*", "Интерфейсы и библиотеки UWP");
    addApp("app_filemgr", "Файловый менеджер", "*FilePicker*", "Интерфейсы и библиотеки UWP");
    addApp("app_vclibs", "Native Runtime & VCLibs", "*VCLibs*", "Интерфейсы и библиотеки UWP");

    // Fast Win32 registry scan to accurately detect installed AppX packages on startup
    std::vector<std::string> fastNames;
    ScanAppxRegistryPackageNames(fastNames);
    for (auto& item : m_appxItems) {
        item.isInstalled = CheckAppxPatternMatch(item.packageNamePattern, fastNames);
    }
}

// -----------------------------------------------------------------------------
// State Detection
// -----------------------------------------------------------------------------
void SystemTweaker::DetectSystemState() {
    // F8 Boot
    m_sysConfig.enableF8Boot = false; // default modern

    // Sticky Keys
    STICKYKEYS sk = { sizeof(sk) };
    if (SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(sk), &sk, 0)) {
        m_sysConfig.disableStickyKeys = !(sk.dwFlags & SKF_STICKYKEYSON) && !(sk.dwFlags & SKF_HOTKEYACTIVE);
    }

    // Classic Photo Viewer
    std::wstring curViewer = RegGetString(HKEY_CLASSES_ROOT, L"PhotoViewer.FileAssoc.Tiff\\shell\\open\\command", L"");
    m_sysConfig.classicPhotoViewer = !curViewer.empty();

    // Auto restart shell
    DWORD autoRestart = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"AutoRestartShell", 1);
    m_sysConfig.restartShellOnCrash = (autoRestart != 0);

    // SvcHost grouping
    DWORD svchostSplit = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SvcHostSplitThresholdInKB", 0x380000);
    m_sysConfig.svchostGrouping = (svchostSplit > 0x380000 || svchostSplit == 0x40000000);

    // Remote Assistance
    DWORD allowHelp = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Remote Assistance", L"fAllowToGetHelp", 1);
    m_sysConfig.disableRemoteAssistance = (allowHelp == 0);

    // Auto end tasks
    std::wstring endTasks = RegGetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"AutoEndTasks");
    m_sysConfig.autoEndTasks = (endTasks == L"1");

    // Hung app timeouts
    std::wstring hungTimeout = RegGetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"HungAppTimeout");
    m_sysConfig.reduceHungAppTimeout = false;
    try {
        if (!hungTimeout.empty()) {
            m_sysConfig.reduceHungAppTimeout = (std::stoi(hungTimeout) <= 2000);
        }
    } catch (...) {}
    m_sysConfig.autoKillHungApps = m_sysConfig.reduceHungAppTimeout;

    // Persist browsers
    DWORD persistBrowsers = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"PersistBrowsers", 0);
    m_sysConfig.disablePersistBrowsers = (persistBrowsers == 0);

    // BSOD auto restart
    DWORD autoReboot = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CrashControl", L"AutoReboot", 1);
    m_sysConfig.disableBsodAutoRestart = (autoReboot == 0);

    // Safe mode MSI
    std::wstring msiSafe = RegGetString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal\\MSIServer", L"");
    m_sysConfig.enableMsiSafeMode = (msiSafe == L"Service");

    // Ctrl+Alt+Del logon
    DWORD disableCAD = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"DisableCAD", 1);
    m_sysConfig.requireCtrlAltDel = (disableCAD == 0);

    // Hide users on login
    DWORD hideUsers = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"dontdisplaylastusername", 0);
    m_sysConfig.hideUserAccountsOnLogon = (hideUsers == 1);

    // Power Throttling
    DWORD pwrThrottle = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff", 0);
    m_sysConfig.disablePowerThrottling = (pwrThrottle == 1);

    // Hibernation & Fast Startup
    DWORD hib = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", 1);
    DWORD fastBoot = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled", 1);
    m_sysConfig.disableHibernation = (hib == 0 && fastBoot == 0);

    // Event logs tracing
    DWORD evtApp = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\EventLog-Application", L"Start", 1);
    m_sysConfig.disableAllEventLogs = (evtApp == 0);
}

void SystemTweaker::DetectSecurityState() {
    // Telemetry
    DWORD telem = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", 1);
    m_secConfig.disableAllTelemetry = (telem == 0);

    // Hidden Monitoring
    DWORD pubAct = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"PublishUserActivities", 1);
    DWORD actFeed = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"EnableActivityFeed", 1);
    m_secConfig.disableHiddenMonitoring = (pubAct == 0 && actFeed == 0);

    // Security notifications
    DWORD secNotif = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Defender Security Center\\Notifications", L"DisableNotifications", 0);
    m_secConfig.disableSecurityCenterNotifications = (secNotif == 1);

    // UAC
    DWORD lua = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableLUA", 1);
    m_secConfig.disableUAC = (lua == 0);

    // USB AutoRun
    DWORD noAutoRun = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoDriveTypeAutoRun", 0);
    m_secConfig.disableUsbAutorun = (noAutoRun == 0xFF);

    // Meltdown / Spectre CPU mitigations
    DWORD featOverride = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"FeatureSettingsOverride", 0);
    m_secConfig.disableCpuMitigations = (featOverride == 3);

    // VBS & HVCI Core Isolation
    DWORD vbs = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"EnableVirtualizationBasedSecurity", 0);
    DWORD hvci = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled", 0);
    m_secConfig.disableVbsHvci = (vbs == 0 && hvci == 0);

    // TTL
    DWORD ttl = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"DefaultTTL", 128);
    m_secConfig.hideRealTTL = (ttl == 64 || ttl == 65);

    // MS Edge Prelaunch
    DWORD prelaunch = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\MicrosoftEdge\\Main", L"AllowPrelaunch", 1);
    m_secConfig.preventEdgeAutorun = (prelaunch == 0);

    // EXE warnings
    std::wstring lowRisk = RegGetString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Associations", L"LowRiskFileTypes");
    m_secConfig.disableExeWarnings = (!lowRisk.empty() && lowRisk.find(L".exe") != std::wstring::npos);

    // Download warnings (Attachment Zone Information)
    DWORD saveZone = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Attachments", L"SaveZoneInformation", 2);
    m_secConfig.disableDownloadWarnings = (saveZone == 1);

    // Browser warnings (SmartScreen / Phishing Filter)
    DWORD edgePhish = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\MicrosoftEdge\\PhishingFilter", L"EnabledV9", 1);
    DWORD sysSmartScreen = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"EnableSmartScreen", 1);
    m_secConfig.disableBrowserWarnings = (edgePhish == 0 || sysSmartScreen == 0);

    // Facebook / Meta sync
    DWORD fbSync = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"DisableFacebookSync", 0);
    m_secConfig.blockFacebookSync = (fbSync == 1);

    // Outlook sync
    DWORD olSync = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Office\\16.0\\Outlook\\Options\\General", L"DisableContactSync", 0);
    m_secConfig.blockOutlookSync = (olSync == 1);

    // Skype sync
    DWORD skypeSync = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"DisableSkypeSync", 0);
    m_secConfig.blockSkypeSync = (skypeSync == 1);
}

// -----------------------------------------------------------------------------
// Service Controller Handling
// -----------------------------------------------------------------------------
void SystemTweaker::RefreshServiceStates() {
    ScopedScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));

    std::vector<std::vector<std::wstring>> allWinNames;
    {
        std::lock_guard<std::mutex> lock(m_servicesMutex);
        allWinNames.reserve(m_services.size());
        for (const auto& s : m_services) {
            allWinNames.push_back(s.winServiceNames);
        }
    }

    struct SvcEval {
        bool disabled = false;
        DWORD currentState = SERVICE_STOPPED;
        std::string statusText = "Остановлена";
    };

    std::vector<SvcEval> evals(allWinNames.size());
    for (size_t i = 0; i < allWinNames.size(); ++i) {
        bool allDisabled = true;
        bool anyRunning = false;
        DWORD bestState = SERVICE_STOPPED;

        for (const auto& svcName : allWinNames[i]) {
            bool thisDisabled = false;

            if (scm) {
                ScopedScHandle hSvc(OpenServiceW(scm, svcName.c_str(), SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS));
                if (hSvc) {
                    DWORD bytesNeeded = 0;
                    QueryServiceConfigW(hSvc, nullptr, 0, &bytesNeeded);
                    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                        std::vector<BYTE> buf(bytesNeeded);
                        QUERY_SERVICE_CONFIGW* pConfig = (QUERY_SERVICE_CONFIGW*)buf.data();
                        if (QueryServiceConfigW(hSvc, pConfig, bytesNeeded, &bytesNeeded)) {
                            if (pConfig->dwStartType == SERVICE_DISABLED) {
                                thisDisabled = true;
                            }
                        }
                    }

                    SERVICE_STATUS_PROCESS ssp = { 0 };
                    DWORD sspNeeded = 0;
                    if (QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &sspNeeded)) {
                        if (ssp.dwCurrentState == SERVICE_RUNNING) {
                            anyRunning = true;
                            bestState = SERVICE_RUNNING;
                            thisDisabled = false;
                        } else if (ssp.dwCurrentState != SERVICE_STOPPED && bestState == SERVICE_STOPPED) {
                            bestState = ssp.dwCurrentState;
                        }
                    }
                }
            }

            // Fallback / validation via Registry HKLM\SYSTEM\CurrentControlSet\Services\<svcName>\Start
            if (!thisDisabled && !anyRunning) {
                std::wstring svcKey = L"SYSTEM\\CurrentControlSet\\Services\\" + svcName;
                DWORD startVal = RegGetDword(HKEY_LOCAL_MACHINE, svcKey, L"Start", 0);
                if (startVal == 4) {
                    thisDisabled = true;
                }
            }

            if (!thisDisabled) {
                allDisabled = false;
            }
        }

        evals[i].disabled = allDisabled;
        evals[i].currentState = anyRunning ? SERVICE_RUNNING : (allDisabled ? SERVICE_STOPPED : bestState);
        if (anyRunning) {
            evals[i].statusText = "Работает";
        } else if (allDisabled) {
            evals[i].statusText = "Отключена";
        } else {
            evals[i].statusText = "Остановлена";
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_servicesMutex);
        for (size_t i = 0; i < evals.size() && i < m_services.size(); ++i) {
            m_services[i].disabled = evals[i].disabled;
            m_services[i].currentServiceState = evals[i].currentState;
            m_services[i].statusText = evals[i].statusText;
            m_services[i].isRunning = (evals[i].currentState == SERVICE_RUNNING);
            m_services[i].status = evals[i].statusText;
        }
    }
}

bool SystemTweaker::ApplyServiceState(ServiceItemInfo& item, bool disable) {
    ScopedScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
    if (!scm) {
        scm.reset(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    }

    bool success = true;
    DWORD targetStart = disable ? SERVICE_DISABLED : (item.defaultStartType == SERVICE_DISABLED ? SERVICE_DEMAND_START : item.defaultStartType);
    DWORD regStartVal = disable ? 4 : (targetStart == SERVICE_AUTO_START ? 2 : 3);
    bool anyRunning = false;

    for (const auto& svcName : item.winServiceNames) {
        if (scm) {
            ScopedScHandle hSvc(OpenServiceW(scm, svcName.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS));
            if (hSvc) {
                if (!ChangeServiceConfigW(hSvc, SERVICE_NO_CHANGE, targetStart, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
                    success = false;
                }
                if (disable) {
                    SERVICE_STATUS_PROCESS ssp;
                    DWORD bytesNeeded = 0;
                    if (QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                        if (ssp.dwCurrentState != SERVICE_STOPPED && ssp.dwCurrentState != SERVICE_STOP_PENDING) {
                            // Enumerate and stop dependent services
                            DWORD dwBytesNeeded = 0;
                            DWORD dwCount = 0;
                            if (!EnumDependentServicesW(hSvc, SERVICE_ACTIVE, nullptr, 0, &dwBytesNeeded, &dwCount)) {
                                if (GetLastError() == ERROR_MORE_DATA && dwBytesNeeded > 0) {
                                    std::vector<BYTE> depBuf(dwBytesNeeded);
                                    ENUM_SERVICE_STATUSW* lpDependencies = (ENUM_SERVICE_STATUSW*)depBuf.data();
                                    if (EnumDependentServicesW(hSvc, SERVICE_ACTIVE, lpDependencies, dwBytesNeeded, &dwBytesNeeded, &dwCount)) {
                                        for (DWORD d = 0; d < dwCount; ++d) {
                                            ScopedScHandle hDep(OpenServiceW(scm, lpDependencies[d].lpServiceName, SERVICE_STOP));
                                            if (hDep) {
                                                SERVICE_STATUS depStatus;
                                                ControlService(hDep, SERVICE_CONTROL_STOP, &depStatus);
                                            }
                                        }
                                    }
                                }
                            }
                            SERVICE_STATUS status;
                            ControlService(hSvc, SERVICE_CONTROL_STOP, &status);
                        }
                    }
                } else {
                    // Attempt to start the service
                    if (StartServiceW(hSvc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                        anyRunning = true;
                    }

                    SERVICE_STATUS_PROCESS ssp = { 0 };
                    DWORD bytesNeeded = 0;
                    if (QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                        if (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING) {
                            anyRunning = true;
                        }
                    }
                }
            }
        }

        // Direct Registry configuration fallback / reinforcement for per-user and tamper-protected services
        std::wstring svcKey = L"SYSTEM\\CurrentControlSet\\Services\\" + svcName;
        RegSetDword(HKEY_LOCAL_MACHINE, svcKey, L"Start", regStartVal);

        // Also check if this is a per-user template service and update active per-user instances (e.g. WpnUserService_44b89)
        ScopedHKey hServicesRoot;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", 0, KEY_READ | KEY_WRITE, hServicesRoot.put()) == ERROR_SUCCESS) {
            DWORD subKeyIndex = 0;
            wchar_t subKeyName[256];
            DWORD subKeyNameLen = 256;
            std::wstring prefix = svcName + L"_";
            while (RegEnumKeyExW(hServicesRoot, subKeyIndex++, subKeyName, &subKeyNameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                if (_wcsnicmp(subKeyName, prefix.c_str(), prefix.size()) == 0) {
                    ScopedHKey hInstKey;
                    if (RegOpenKeyExW(hServicesRoot, subKeyName, 0, KEY_SET_VALUE, hInstKey.put()) == ERROR_SUCCESS) {
                        RegSetValueExW(hInstKey, L"Start", 0, REG_DWORD, (const BYTE*)&regStartVal, sizeof(DWORD));
                    }
                    if (scm) {
                        if (disable) {
                            // Do not terminate active user push notification service live to prevent breaking desktop apps
                            if (_wcsicmp(svcName.c_str(), L"WpnUserService") != 0) {
                                ScopedScHandle hInstSvc(OpenServiceW(scm, subKeyName, SERVICE_STOP));
                                if (hInstSvc) {
                                    SERVICE_STATUS instStat;
                                    ControlService(hInstSvc, SERVICE_CONTROL_STOP, &instStat);
                                }
                            }
                        } else {
                            ScopedScHandle hInstSvc(OpenServiceW(scm, subKeyName, SERVICE_START | SERVICE_QUERY_STATUS));
                            if (hInstSvc) {
                                if (StartServiceW(hInstSvc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                                    anyRunning = true;
                                }
                                SERVICE_STATUS_PROCESS ssp = { 0 };
                                DWORD bytesNeeded = 0;
                                if (QueryServiceStatusEx(hInstSvc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &bytesNeeded)) {
                                    if (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING) {
                                        anyRunning = true;
                                    }
                                }
                            }
                        }
                    }
                }
                subKeyNameLen = 256;
            }
        }
    }

    if (disable) {
        item.isRunning = false;
        item.disabled = true;
        item.status = "Отключена";
        item.statusText = "Отключена";
        item.currentServiceState = SERVICE_STOPPED;
    } else {
        item.disabled = false;
        if (anyRunning) {
            item.isRunning = true;
            item.status = "Работает";
            item.statusText = "Работает";
            item.currentServiceState = SERVICE_RUNNING;
        } else {
            item.isRunning = false;
            item.status = "Включена";
            item.statusText = "Включена";
            item.currentServiceState = SERVICE_STOPPED;
        }
    }

    return success;
}

int SystemTweaker::GetDisabledServicesCount() const {
    std::lock_guard<std::mutex> lock(m_servicesMutex);
    int cnt = 0;
    for (const auto& item : m_services) {
        if (item.disabled) cnt++;
    }
    return cnt;
}

bool SystemTweaker::EnableAllServices(std::string& outMsg) {
    int count = 0;
    for (auto& item : m_services) {
        if (item.disabled) {
            ApplyServiceState(item, false);
            count++;
        }
    }
    outMsg = "Включено служб по умолчанию: " + std::to_string(count);
    return true;
}

bool SystemTweaker::DisableRecommendedServices(std::string& outMsg) {
    int count = 0;
    for (auto& item : m_services) {
        if (item.recommendedDisable && !item.disabled) {
            ApplyServiceState(item, true);
            count++;
        }
    }
    outMsg = "Отключено киберспортивных служб: " + std::to_string(count);
    return true;
}

// -----------------------------------------------------------------------------
// System Tweaks Execution
// -----------------------------------------------------------------------------
bool SystemTweaker::ApplySingleSystemTweak(const std::string& tweakId, bool val, std::string& outMsg) {
    if (tweakId == "maxOsPerf") {
        return ApplyAllOsPerfSubTweaks(val, outMsg);
    }
    else if (tweakId == "maxSsdPerf") {
        return ApplyAllSsdPerfSubTweaks(val, outMsg);
    }
    else if (tweakId == "enableF8Boot") {
        m_sysConfig.enableF8Boot = val;
        RunCmdSilent(val ? L"bcdedit /set {default} bootmenupolicy legacy" : L"bcdedit /set {default} bootmenupolicy standard");
        outMsg = val ? "Меню загрузки по F8 включено" : "Меню загрузки по F8 отключено";
    }
    else if (tweakId == "classicPhotoViewer") {
        m_sysConfig.classicPhotoViewer = val;
        if (val) {
            const wchar_t* dll = L"%SystemRoot%\\System32\\rundll32.exe \"%ProgramFiles%\\Windows Photo Viewer\\PhotoViewer.dll\", ImageView_Fullscreen %1";
            RegSetString(HKEY_CLASSES_ROOT, L"PhotoViewer.FileAssoc.Tiff\\shell\\open\\command", L"", dll);
            RegSetString(HKEY_CLASSES_ROOT, L"PhotoViewer.FileAssoc.Bitmap\\shell\\open\\command", L"", dll);
            RegSetString(HKEY_CLASSES_ROOT, L"PhotoViewer.FileAssoc.Jpeg\\shell\\open\\command", L"", dll);
            RegSetString(HKEY_CLASSES_ROOT, L"PhotoViewer.FileAssoc.Png\\shell\\open\\command", L"", dll);
        }
        outMsg = val ? "Классический просмотр фотографий активирован" : "Просмотр фотографий по умолчанию";
    }
    else if (tweakId == "disableStickyKeys") {
        m_sysConfig.disableStickyKeys = val;
        STICKYKEYS sk = { sizeof(sk) };
        sk.dwFlags = val ? (SKF_AVAILABLE | SKF_CONFIRMHOTKEY) : (SKF_AVAILABLE | SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY);
        SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof(sk), &sk, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        outMsg = val ? "Залипание клавиш отключено" : "Залипание клавиш включено";
    }
    else if (tweakId == "restartShellOnCrash") {
        m_sysConfig.restartShellOnCrash = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"AutoRestartShell", val ? 1 : 0);
        outMsg = val ? "Авто-рестарт Проводника включен" : "Авто-рестарт отключен";
    }
    else if (tweakId == "svchostGrouping") {
        m_sysConfig.svchostGrouping = val;
        // Group processes by setting threshold above RAM (e.g. 1024GB) or split (3.5GB)
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SvcHostSplitThresholdInKB", val ? 0x40000000 : 0x380000);
        outMsg = val ? "Группировка процессов SvcHost включена" : "Разделение процессов SvcHost по умолчанию";
    }
    else if (tweakId == "disableRemoteAssistance") {
        m_sysConfig.disableRemoteAssistance = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Remote Assistance", L"fAllowToGetHelp", val ? 0 : 1);
        outMsg = val ? "Удаленный помощник запрещен" : "Удаленный помощник разрешен";
    }
    else if (tweakId == "allowNetFramework4") {
        m_sysConfig.allowNetFramework4 = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\.NETFramework\\v4.0.30319", L"SchUseStrongCrypto", val ? 1 : 0);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Microsoft\\.NETFramework\\v4.0.30319", L"SchUseStrongCrypto", val ? 1 : 0);
        outMsg = val ? "Поддержка .NET Framework 4 настроена" : ".NET Framework по умолчанию";
    }
    else if (tweakId == "autoEndTasks") {
        m_sysConfig.autoEndTasks = val;
        RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"AutoEndTasks", val ? L"1" : L"0");
        outMsg = val ? "Авто-завершение задач при выключении включено" : "Авто-завершение выключено";
    }
    else if (tweakId == "reduceHungAppTimeout" || tweakId == "autoKillHungApps") {
        m_sysConfig.reduceHungAppTimeout = val;
        m_sysConfig.autoKillHungApps = val;
        RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"HungAppTimeout", val ? L"1000" : L"5000");
        RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WaitToKillAppTimeout", val ? L"2000" : L"10000");
        RegSetString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"WaitToKillServiceTimeout", val ? L"2000" : L"5000");
        outMsg = val ? "Таймаут зависших процессов уменьшен" : "Таймаут процессов по умолчанию";
    }
    else if (tweakId == "disablePersistBrowsers") {
        m_sysConfig.disablePersistBrowsers = val;
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"PersistBrowsers", val ? 0 : 1);
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"RestartApps", val ? 0 : 1);
        outMsg = val ? "Автозапуск незакрытых программ отключен" : "Автозапуск сеанса по умолчанию";
    }
    else if (tweakId == "disableFinishSetup") {
        m_sysConfig.disableFinishSetup = val;
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-310093Enabled", val ? 0 : 1);
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\UserProfileEngagement", L"ScoobeSystemSettingEnabled", val ? 0 : 1);
        outMsg = val ? "Предложение завершить настройку отключено" : "Предложение настройки по умолчанию";
    }
    else if (tweakId == "verboseStatus") {
        m_sysConfig.verboseStatus = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"VerboseStatus", val ? 1 : 0);
        outMsg = val ? "Отображение выполняемых команд включено" : "Отображение команд отключено";
    }
    else if (tweakId == "hideUserAccountsOnLogon") {
        m_sysConfig.hideUserAccountsOnLogon = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"dontdisplaylastusername", val ? 1 : 0);
        outMsg = val ? "Учетные записи на экране входа скрыты" : "Учетные записи отображаются";
    }
    else if (tweakId == "requireCtrlAltDel") {
        m_sysConfig.requireCtrlAltDel = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"DisableCAD", val ? 0 : 1);
        outMsg = val ? "Требование Ctrl+Alt+Del включено" : "Вход без Ctrl+Alt+Del";
    }
    else if (tweakId == "enableMsiSafeMode") {
        m_sysConfig.enableMsiSafeMode = val;
        if (val) {
            RegSetString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal\\MSIServer", L"", L"Service");
            RegSetString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Network\\MSIServer", L"", L"Service");
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal\\MSIServer", L"");
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Network\\MSIServer", L"");
        }
        outMsg = val ? "Служба MSI Installer в Safe Mode разрешена" : "MSI в Safe Mode по умолчанию";
    }
    else if (tweakId == "disableBsodAutoRestart") {
        m_sysConfig.disableBsodAutoRestart = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CrashControl", L"AutoReboot", val ? 0 : 1);
        outMsg = val ? "Автоперезагрузка при BSOD отключена" : "Автоперезагрузка при BSOD включена";
    }
    else if (tweakId == "powerShellAcceleration") {
        m_sysConfig.powerShellAcceleration = val;
        if (val) {
            RunCmdSilent(L"powershell.exe -NoProfile -Command \"[appdomain]::currentdomain.getassemblies() | foreach {ngen install $_.location -nologo}\"");
        }
        outMsg = val ? "Ускорение скриптов PowerShell выполнено" : "PowerShell по умолчанию";
    }
    else if (tweakId == "disablePowerThrottling") {
        m_sysConfig.disablePowerThrottling = val;
        m_osPerfSub.disablePowerThrottling = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff", val ? 1 : 0);
        outMsg = val ? "Power Throttling процессора отключен (EcoQoS заблокирован)" : "Power Throttling по умолчанию";
    }
    else if (tweakId == "disableHibernation") {
        m_sysConfig.disableHibernation = val;
        m_osPerfSub.disableHibernation = val;
        if (val) {
            RunCmdSilent(L"powercfg -h off");
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", 0);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled", 0);
        } else {
            RunCmdSilent(L"powercfg -h on");
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", 1);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", L"HiberbootEnabled", 1);
        }
        outMsg = val ? "Гибернация и Fast Startup отключены (SSD освобожден, таймеры синхронны)" : "Гибернация и Fast Startup включены";
    }
    else if (tweakId == "disableAllEventLogs") {
        m_sysConfig.disableAllEventLogs = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\EventLog-Application", L"Start", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\EventLog-System", L"Start", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\EventLog-Security", L"Start", val ? 0 : 1);
        outMsg = val ? "Создание логов системных журналов отключено" : "Системные журналы ведутся";
    }
    return true;
}

bool SystemTweaker::ApplySystemTweaks(std::string& outMsg) {
    ApplySingleSystemTweak("maxOsPerf", m_sysConfig.maxOsPerf, outMsg);
    ApplySingleSystemTweak("maxSsdPerf", m_sysConfig.maxSsdPerf, outMsg);
    ApplySingleSystemTweak("disablePowerThrottling", m_sysConfig.disablePowerThrottling, outMsg);
    ApplySingleSystemTweak("disableHibernation", m_sysConfig.disableHibernation, outMsg);
    ApplySingleSystemTweak("enableF8Boot", m_sysConfig.enableF8Boot, outMsg);
    ApplySingleSystemTweak("classicPhotoViewer", m_sysConfig.classicPhotoViewer, outMsg);
    ApplySingleSystemTweak("disableStickyKeys", m_sysConfig.disableStickyKeys, outMsg);
    ApplySingleSystemTweak("restartShellOnCrash", m_sysConfig.restartShellOnCrash, outMsg);
    ApplySingleSystemTweak("svchostGrouping", m_sysConfig.svchostGrouping, outMsg);
    ApplySingleSystemTweak("disableRemoteAssistance", m_sysConfig.disableRemoteAssistance, outMsg);
    ApplySingleSystemTweak("autoEndTasks", m_sysConfig.autoEndTasks, outMsg);
    ApplySingleSystemTweak("reduceHungAppTimeout", m_sysConfig.reduceHungAppTimeout, outMsg);
    ApplySingleSystemTweak("disablePersistBrowsers", m_sysConfig.disablePersistBrowsers, outMsg);
    ApplySingleSystemTweak("disableFinishSetup", m_sysConfig.disableFinishSetup, outMsg);
    ApplySingleSystemTweak("disableBsodAutoRestart", m_sysConfig.disableBsodAutoRestart, outMsg);
    ApplySingleSystemTweak("disableAllEventLogs", m_sysConfig.disableAllEventLogs, outMsg);
    outMsg = "Все системные настройки успешно применены";
    return true;
}

// -----------------------------------------------------------------------------
// Security Tweaks Execution
// -----------------------------------------------------------------------------
bool SystemTweaker::ApplySingleSecurityTweak(const std::string& tweakId, bool val, std::string& outMsg) {
    if (tweakId == "disableAllTelemetry") {
        return ApplyAllTelemetrySubTweaks(val, outMsg);
    }
    else if (tweakId == "disableHiddenMonitoring") {
        m_secConfig.disableHiddenMonitoring = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"PublishUserActivities", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"UploadUserActivities", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"EnableActivityFeed", val ? 0 : 1);
        outMsg = val ? "Скрытый мониторинг системы отключен" : "Мониторинг активности по умолчанию";
    }
    else if (tweakId == "removeSpyServices") {
        m_secConfig.removeSpyServices = val;
        for (auto& s : m_services) {
            if (s.id == "svc_diagtrack" || s.id == "svc_dps" || s.id == "svc_wersvc") {
                ApplyServiceState(s, val);
            }
        }
        outMsg = val ? "Шпионские службы остановлены и отключены" : "Службы телеметрии восстановлены";
    }
    else if (tweakId == "blockTelemetryDomains") {
        m_secConfig.blockTelemetryDomains = val;
        std::wstring hostsPath = L"C:\\Windows\\System32\\drivers\\etc\\hosts";
        if (val) {
            std::ofstream hosts(hostsPath, std::ios::app);
            if (hosts.is_open()) {
                hosts << "\n# Windows Esports Suite Telemetry Block\n";
                hosts << "0.0.0.0 vortex.data.microsoft.com\n";
                hosts << "0.0.0.0 telemetry.microsoft.com\n";
                hosts << "0.0.0.0 watson.telemetry.microsoft.com\n";
                hosts << "0.0.0.0 diagnostics.support.microsoft.com\n";
                hosts.close();
            }
        }
        outMsg = val ? "Домены телеметрии заблокированы" : "Блокировка доменов снята";
    }
    else if (tweakId == "disableSecurityCenterNotifications") {
        m_secConfig.disableSecurityCenterNotifications = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Defender Security Center\\Notifications", L"DisableNotifications", val ? 1 : 0);
        outMsg = val ? "Оповещения Центра безопасности отключены" : "Оповещения безопасности включены";
    }
    else if (tweakId == "disableExeWarnings") {
        m_secConfig.disableExeWarnings = val;
        if (val) {
            RegSetString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Associations", L"LowRiskFileTypes", L".exe;.bat;.cmd;.msi;.reg");
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Associations", L"LowRiskFileTypes");
        }
        outMsg = val ? "Предупреждения при запуске EXE отключены" : "Предупреждения EXE включены";
    }
    else if (tweakId == "disableDownloadWarnings") {
        m_secConfig.disableDownloadWarnings = val;
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Attachments", L"SaveZoneInformation", val ? 1 : 2);
        outMsg = val ? "Предупреждения при загрузке файлов отключены" : "Предупреждения загрузки включены";
    }
    else if (tweakId == "disableUAC") {
        m_secConfig.disableUAC = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableLUA", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"ConsentPromptBehaviorAdmin", val ? 0 : 5);
        outMsg = val ? "UAC (Контроль учетных записей) отключен" : "UAC включен по умолчанию";
    }
    else if (tweakId == "disableUsbAutorun") {
        m_secConfig.disableUsbAutorun = val;
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoDriveTypeAutoRun", val ? 0xFF : 0x91);
        outMsg = val ? "Автозапуск USB накопителей отключен" : "Автозапуск USB по умолчанию";
    }
    else if (tweakId == "disableCpuMitigations") {
        m_secConfig.disableCpuMitigations = val;
        // FeatureSettingsOverride = 3, Mask = 3 disables Spectre, Meltdown, Downfall mitigation penalties
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"FeatureSettingsOverride", val ? 3 : 0);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"FeatureSettingsOverrideMask", val ? 3 : 0);
        outMsg = val ? "Фиксы Spectre/Meltdown отключены (Максимальный FPS)" : "Безопасность процессора по умолчанию";
    }
    else if (tweakId == "disableVbsHvci") {
        m_secConfig.disableVbsHvci = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"EnableVirtualizationBasedSecurity", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\CredentialGuard", L"Enabled", val ? 0 : 1);
        outMsg = val ? "Изоляция ядра (VBS) и HVCI отключены (Прирост IPC в играх)" : "Изоляция ядра (VBS) включена";
    }
    else if (tweakId == "preventEdgeAutorun") {
        m_secConfig.preventEdgeAutorun = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\MicrosoftEdge\\Main", L"AllowPrelaunch", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\MicrosoftEdge\\TabPreloader", L"AllowTabPreloading", val ? 0 : 1);
        outMsg = val ? "Автозапуск Microsoft Edge запрещен" : "Фоновый запуск Edge разрешен";
    }
    else if (tweakId == "hideRealTTL") {
        m_secConfig.hideRealTTL = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"DefaultTTL", val ? 64 : 128);
        outMsg = val ? "TTL оптимизирован для киберспорта (TTL=64)" : "TTL по умолчанию (128)";
    }
    else if (tweakId == "disableBrowserWarnings") {
        m_secConfig.disableBrowserWarnings = val;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\MicrosoftEdge\\PhishingFilter", L"EnabledV9", val ? 0 : 1);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"EnableSmartScreen", val ? 0 : 1);
        outMsg = val ? "Предупреждения в браузерах отключены" : "Предупреждения в браузерах включены";
    }
    else if (tweakId == "blockFacebookSync") {
        m_secConfig.blockFacebookSync = val;
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"DisableFacebookSync", val ? 1 : 0);
        outMsg = val ? "Синхронизация контактов Facebook заблокирована" : "Синхронизация Facebook разрешена";
    }
    else if (tweakId == "blockOutlookSync") {
        m_secConfig.blockOutlookSync = val;
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Office\\16.0\\Outlook\\Options\\General", L"DisableContactSync", val ? 1 : 0);
        outMsg = val ? "Синхронизация контактов Outlook заблокирована" : "Синхронизация Outlook разрешена";
    }
    else if (tweakId == "blockSkypeSync") {
        m_secConfig.blockSkypeSync = val;
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"DisableSkypeSync", val ? 1 : 0);
        outMsg = val ? "Синхронизация контактов Skype заблокирована" : "Синхронизация Skype разрешена";
    }
    return true;
}

bool SystemTweaker::ApplySecurityTweaks(std::string& outMsg) {
    ApplySingleSecurityTweak("disableAllTelemetry", m_secConfig.disableAllTelemetry, outMsg);
    ApplySingleSecurityTweak("disableHiddenMonitoring", m_secConfig.disableHiddenMonitoring, outMsg);
    ApplySingleSecurityTweak("disableSecurityCenterNotifications", m_secConfig.disableSecurityCenterNotifications, outMsg);
    ApplySingleSecurityTweak("disableExeWarnings", m_secConfig.disableExeWarnings, outMsg);
    ApplySingleSecurityTweak("disableDownloadWarnings", m_secConfig.disableDownloadWarnings, outMsg);
    ApplySingleSecurityTweak("disableBrowserWarnings", m_secConfig.disableBrowserWarnings, outMsg);
    ApplySingleSecurityTweak("disableUAC", m_secConfig.disableUAC, outMsg);
    ApplySingleSecurityTweak("disableUsbAutorun", m_secConfig.disableUsbAutorun, outMsg);
    ApplySingleSecurityTweak("disableCpuMitigations", m_secConfig.disableCpuMitigations, outMsg);
    ApplySingleSecurityTweak("disableVbsHvci", m_secConfig.disableVbsHvci, outMsg);
    ApplySingleSecurityTweak("preventEdgeAutorun", m_secConfig.preventEdgeAutorun, outMsg);
    ApplySingleSecurityTweak("hideRealTTL", m_secConfig.hideRealTTL, outMsg);
    ApplySingleSecurityTweak("blockFacebookSync", m_secConfig.blockFacebookSync, outMsg);
    ApplySingleSecurityTweak("blockOutlookSync", m_secConfig.blockOutlookSync, outMsg);
    ApplySingleSecurityTweak("blockSkypeSync", m_secConfig.blockSkypeSync, outMsg);
    outMsg = "Все параметры безопасности успешно обновлены";
    return true;
}

// -----------------------------------------------------------------------------
// AppX Debloat Execution
// -----------------------------------------------------------------------------
static bool IsAppxWhitelisted(const std::string& token) {
    static const std::vector<std::string> whitelist = {
        "startmenuexperiencehost",
        "shellexperiencehost",
        "windows.cbs",
        "cbspreview",
        "textinputhost",
        "immersivecontrolpanel",
        "desktopappinstaller",
        "vclibs",
        "accountscontrol"
    };
    std::string lower;
    for (char c : token) {
        if (c != '*' && c != ' ') lower.push_back(::tolower(c));
    }
    for (const auto& safe : whitelist) {
        if (lower.find(safe) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void SystemTweaker::ScanInstalledAppxPackages() {
    DEBUG_LOG_INFO("Tweaker", "Scanning installed AppX packages...");
    std::vector<std::string> installedNames;

    // 1. Direct, instant Win32 registry enumeration of AppModel / PackageRepository
    ScanAppxRegistryPackageNames(installedNames);

    // 2. Match against known AppX database patterns
    {
        std::lock_guard<std::mutex> lock(m_appxMutex);
        for (auto& item : m_appxItems) {
            item.isInstalled = CheckAppxPatternMatch(item.packageNamePattern, installedNames);
        }
    }
    DEBUG_LOG_SUCCESS("Tweaker", "AppX package scan completed");
}

void SystemTweaker::SelectAllAppx(bool select) {
    for (auto& item : m_appxItems) {
        if (!select) {
            item.selected = false;
        } else {
            if (item.isInstalled &&
                item.category != "Интерфейсы и библиотеки UWP" &&
                item.category != "Антивирус" &&
                item.id != "app_store" &&
                item.id != "app_appinstaller") {
                item.selected = true;
            } else {
                item.selected = false;
            }
        }
    }
}

bool SystemTweaker::ApplyAppxSettings(std::string& outMsg) {
    if (disableStoreAutoUpdate) {
        RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\WindowsStore", L"AutoDownload", 2);
    }
    if (disableStartRecommended) {
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338388Enabled", 0);
        RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SystemPaneSuggestionsEnabled", 0);
    }
    outMsg = "Настройки UWP сохранены";
    return true;
}

bool SystemTweaker::UninstallSelectedAppx(const std::function<void(const std::string&)>& progressCb, std::string& outMsg) {
    if (m_isBusy.load()) {
        outMsg = "Операция уже выполняется";
        return false;
    }

    std::vector<std::pair<std::string, std::string>> targets;
    for (const auto& item : m_appxItems) {
        if (item.selected) {
            targets.push_back({ item.id, item.packageNamePattern });
        }
    }

    if (targets.empty()) {
        outMsg = "Не выбрано ни одного приложения для удаления";
        return false;
    }

    m_isBusy.store(true);
    std::thread([this, targets, progressCb]() {
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto& target = targets[i];
            m_busyStatus = "Удаление (" + std::to_string(i + 1) + "/" + std::to_string(targets.size()) + "): " + target.second;
            if (progressCb) progressCb(m_busyStatus);

            std::stringstream ss(target.second);
            std::string token;
            while (std::getline(ss, token, ';')) {
                std::stringstream ss2(token);
                std::string subToken;
                while (std::getline(ss2, subToken, ',')) {
                    while (!subToken.empty() && (subToken.back() == ' ' || subToken.back() == '\t')) subToken.pop_back();
                    size_t start = 0;
                    while (start < subToken.size() && (subToken[start] == ' ' || subToken[start] == '\t')) start++;
                    if (start >= subToken.size()) continue;
                    std::string pat = subToken.substr(start);

                    if (IsAppxWhitelisted(pat)) {
                        DEBUG_LOG_WARN("AppX", "Skipping protected whitelist package: " + pat);
                        continue;
                    }

                    std::wstring wPat(pat.begin(), pat.end());
                    std::wstring psCmd = L"powershell.exe -NoProfile -Command \""
                                         L"Get-AppxPackage -AllUsers -Name '" + wPat + L"' | Remove-AppxPackage -AllUsers -ErrorAction SilentlyContinue; "
                                         L"Get-AppxPackage -Name '" + wPat + L"' | Remove-AppxPackage -ErrorAction SilentlyContinue; "
                                         L"Get-AppxProvisionedPackage -Online | Where-Object { $_.DisplayName -like '" + wPat + L"' -or $_.PackageName -like '" + wPat + L"' } | Remove-AppxProvisionedPackage -Online -ErrorAction SilentlyContinue\"";
                    RunCmdSilent(psCmd, 30000);
                }
            }

            for (auto& item : m_appxItems) {
                if (item.id == target.first) {
                    item.isInstalled = false;
                    item.selected = false;
                    break;
                }
            }
        }
        // If Snipping Tool or ScreenSketch were removed, ensure Shell doesn't intercept PrntScr
        RegSetDword(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"PrintScreenKeyForSnippingEnabled", 0);
        m_isBusy.store(false);
        m_busyStatus = "Удаление UWP приложений завершено";
        if (progressCb) progressCb(m_busyStatus);
    }).detach();

    outMsg = "Фоновое удаление UWP запущено...";
    return true;
}

bool SystemTweaker::UninstallAllAppx(const std::function<void(const std::string&)>& progressCb, std::string& outMsg) {
    SelectAllAppx(true);
    return UninstallSelectedAppx(progressCb, outMsg);
}

bool SystemTweaker::RestoreAppxPackage(const std::string& appId, std::string& outMsg) {
    std::string displayName;
    std::string pattern;

    {
        std::lock_guard<std::mutex> lock(m_appxMutex);
        bool found = false;
        for (auto& item : m_appxItems) {
            if (item.id == appId) {
                displayName = item.displayName;
                pattern = item.packageNamePattern;
                found = true;
                break;
            }
        }
        if (!found) {
            outMsg = "Приложение не найдено в базе данных";
            return false;
        }
    }

    // 1. Special case: Microsoft Store
    if (appId == "app_store") {
        RunCmdSilent(L"wsreset.exe -i", 15000);
    }

    // 2. Special case: OneDrive
    if (appId == "app_onedrive") {
        wchar_t sysRoot[MAX_PATH] = { 0 };
        if (GetWindowsDirectoryW(sysRoot, MAX_PATH) > 0) {
            std::wstring p1 = std::wstring(sysRoot) + L"\\SysWOW64\\OneDriveSetup.exe";
            std::wstring p2 = std::wstring(sysRoot) + L"\\System32\\OneDriveSetup.exe";
            if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) {
                RunCmdSilent(L"\"" + p1 + L"\" /install", 15000);
            } else if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES) {
                RunCmdSilent(L"\"" + p2 + L"\" /install", 15000);
            }
        }
    }

    // 3. For UWP packages: search WindowsApps for matching manifest and register
    if (appId != "app_onedrive") {
        std::stringstream ss(pattern);
        std::string token;
        while (std::getline(ss, token, ';')) {
            std::stringstream ss2(token);
            std::string subToken;
            while (std::getline(ss2, subToken, ',')) {
                while (!subToken.empty() && (subToken.back() == ' ' || subToken.back() == '\t')) subToken.pop_back();
                size_t start = 0;
                while (start < subToken.size() && (subToken[start] == ' ' || subToken[start] == '\t')) start++;
                if (start >= subToken.size()) continue;
                std::string pat = subToken.substr(start);

                std::string cleanToken;
                for (char c : pat) {
                    if (c != '*' && c != ' ' && c != '\t') {
                        cleanToken.push_back(c);
                    }
                }
                if (cleanToken.empty()) continue;

                std::wstring wPat(pat.begin(), pat.end());
                std::wstring wToken(cleanToken.begin(), cleanToken.end());

                std::wstring psCmd = L"powershell.exe -NoProfile -Command \""
                    L"Get-AppxPackage -AllUsers -Name '" + wPat + L"' | ForEach-Object { Add-AppxPackage -DisableDevelopmentMode -Register \\\"$($_.InstallLocation)\\AppxManifest.xml\\\" -ErrorAction SilentlyContinue }; "
                    L"$p = (Get-ChildItem 'C:\\Program Files\\WindowsApps' -Recurse -Filter 'AppxManifest.xml' -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -like '*" + wToken + L"*' } | Select-Object -First 1); "
                    L"if ($p) { Add-AppxPackage -DisableDevelopmentMode -Register $p.FullName -ErrorAction SilentlyContinue }\"";

                RunCmdSilent(psCmd, 45000);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_appxMutex);
        for (auto& item : m_appxItems) {
            if (item.id == appId) {
                item.isInstalled = true;
                item.selected = false;
                break;
            }
        }
    }

    outMsg = "Приложение «" + (displayName.empty() ? appId : displayName) + "» успешно восстановлено";
    return true;
}

// -----------------------------------------------------------------------------
// System Cleanup Execution (Screenshot 5)
// -----------------------------------------------------------------------------
void SystemTweaker::SetAllCleanupOptions(bool set) {
    m_cleanupOptions.cleanDefenderCache = set;
    m_cleanupOptions.cleanUsbHistory = set;
    m_cleanupOptions.cleanWindowsInstallCache = set;
    m_cleanupOptions.cleanEventLogs = set;
    m_cleanupOptions.cleanSoftwareDistribution = set;
    m_cleanupOptions.cleanTempFiles = set;
    m_cleanupOptions.cleanExplorerThumbnails = set;
    m_cleanupOptions.cleanDiagnosticLogs = set;
    m_cleanupOptions.cleanBrowserCache = set;
    m_cleanupOptions.cleanSystemReports = set;
    m_cleanupOptions.cleanOfficeCache = set;
    m_cleanupOptions.cleanGpuCache = set;
}

bool SystemTweaker::ExecuteCleanup(const CleanupOptions& opts, uint64_t& outBytesFreed, std::string& outMsg) {
    outBytesFreed = 0;

    bool anyChecked = opts.cleanDefenderCache || opts.cleanUsbHistory || opts.cleanWindowsInstallCache ||
                      opts.cleanEventLogs || opts.cleanSoftwareDistribution || opts.cleanTempFiles ||
                      opts.cleanExplorerThumbnails || opts.cleanDiagnosticLogs || opts.cleanBrowserCache ||
                      opts.cleanSystemReports || opts.cleanOfficeCache || opts.cleanGpuCache;

    if (!anyChecked) {
        outMsg = "Выберите хотя бы один пункт для очистки!";
        return false;
    }

    wchar_t localAppData[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData);
    std::wstring localApp(localAppData);

    wchar_t appDataBuf[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataBuf);
    std::wstring roamingApp(appDataBuf);

    wchar_t profileBuf[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, profileBuf);
    std::wstring userProfile(profileBuf);
    std::wstring localLow = userProfile + L"\\AppData\\LocalLow";

    wchar_t tempPathBuf[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tempPathBuf);
    std::wstring userTemp(tempPathBuf);

    std::atomic<uint64_t> totalFreed{ 0 };
    std::vector<std::future<void>> tasks;

    // 1. Временные файлы (%TEMP%, Windows\Temp, Prefetch) - fast parallel execution
    if (opts.cleanTempFiles) {
        tasks.push_back(std::async(std::launch::async, [&, userTemp, localApp]() {
            uint64_t freed = DeleteDirectoriesParallel({
                userTemp,
                L"C:\\Windows\\Temp",
                L"C:\\Windows\\SystemTemp",
                L"C:\\Windows\\Prefetch",
                localApp + L"\\Microsoft\\Windows\\INetCache",
                localApp + L"\\Microsoft\\Windows\\Temporary Internet Files"
            }, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 2. Кэш эскизов Проводника (Thumbnails)
    if (opts.cleanExplorerThumbnails) {
        tasks.push_back(std::async(std::launch::async, [&, localApp]() {
            uint64_t freed = DeleteDirectoryFiles(localApp + L"\\Microsoft\\Windows\\Explorer", false);
            freed += DeleteDirectoryFiles(localApp + L"\\IconCache.db", false);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 3. Кэш обновлений SoftwareDistribution - direct folder deletion without blocking service stops
    if (opts.cleanSoftwareDistribution) {
        tasks.push_back(std::async(std::launch::async, [&]() {
            uint64_t freed = DeleteDirectoriesParallel({
                L"C:\\Windows\\SoftwareDistribution\\Download",
                L"C:\\Windows\\SoftwareDistribution\\DeliveryOptimization",
                L"C:\\Windows\\SoftwareDistribution\\PostRebootEventCache.V2",
                L"C:\\Windows\\SoftwareDistribution\\ScanFile",
                L"C:\\ProgramData\\Microsoft\\Network\\Downloader"
            }, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 4. Журналы событий Windows (Event Logs) - direct parallel clearing of primary channels + fast wevtutil batching with strict 2-second timeout
    if (opts.cleanEventLogs) {
        tasks.push_back(std::async(std::launch::async, [&]() {
            uint64_t logsBefore = 0;
            {
                WIN32_FIND_DATAW ffd;
                HANDLE hFind = FindFirstFileExW(L"C:\\Windows\\System32\\winevt\\Logs\\*.evtx", FindExInfoBasic, &ffd,
                                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        logsBefore += ((uint64_t)ffd.nFileSizeHigh << 32) | (uint64_t)ffd.nFileSizeLow;
                    } while (FindNextFileW(hFind, &ffd));
                    FindClose(hFind);
                }
            }

            // Direct parallel clearing of primary channels
            const std::vector<std::wstring> primaryChannels = {
                L"Application", L"Security", L"System", L"Setup", L"ForwardedEvents"
            };

            HMODULE hWevt = LoadLibraryW(L"wevtapi.dll");
            typedef BOOL (WINAPI *PEvtClearLog)(HANDLE Session, LPCWSTR ChannelPath, LPCWSTR TargetFilePath, DWORD Flags);
            typedef HANDLE (WINAPI *PEvtOpenChannelEnum)(HANDLE Session, DWORD Flags);
            typedef BOOL (WINAPI *PEvtNextChannelPath)(HANDLE ChannelEnum, DWORD ChannelPathBufferSize, LPWSTR ChannelPathBuffer, PDWORD ChannelPathBufferUsed);
            typedef BOOL (WINAPI *PEvtClose)(HANDLE Object);

            PEvtClearLog pEvtClearLog = hWevt ? (PEvtClearLog)GetProcAddress(hWevt, "EvtClearLog") : nullptr;
            PEvtOpenChannelEnum pEvtOpenChannelEnum = hWevt ? (PEvtOpenChannelEnum)GetProcAddress(hWevt, "EvtOpenChannelEnum") : nullptr;
            PEvtNextChannelPath pEvtNextChannelPath = hWevt ? (PEvtNextChannelPath)GetProcAddress(hWevt, "EvtNextChannelPath") : nullptr;
            PEvtClose pEvtClose = hWevt ? (PEvtClose)GetProcAddress(hWevt, "EvtClose") : nullptr;

            for (const auto& ch : primaryChannels) {
                std::thread([ch, pEvtClearLog]() {
                    HANDLE hLog = OpenEventLogW(NULL, ch.c_str());
                    if (hLog) {
                        ClearEventLogW(hLog, NULL);
                        CloseEventLog(hLog);
                    }
                    if (pEvtClearLog) {
                        pEvtClearLog(NULL, ch.c_str(), NULL, 0);
                    }
                }).detach();
            }

            // Fast batch clearing with wevtutil under strict 1.5-second timeout
            std::wstring wevtBatch = L"wevtutil cl Application & wevtutil cl Security & wevtutil cl System & wevtutil cl Setup & "
                                    L"wevtutil cl ForwardedEvents & "
                                    L"wevtutil cl \"Microsoft-Windows-Diagnostics-Performance/Operational\" & "
                                    L"wevtutil cl \"Microsoft-Windows-TaskScheduler/Operational\" & "
                                    L"wevtutil cl \"Microsoft-Windows-WindowsUpdateClient/Operational\"";
            RunCmdSilent(wevtBatch, 1500);

            // Fast enumeration of additional channels if wevtapi loaded with strict 800ms deadline
            if (hWevt && pEvtOpenChannelEnum && pEvtNextChannelPath && pEvtClearLog && pEvtClose) {
                HANDLE hEnum = pEvtOpenChannelEnum(NULL, 0);
                if (hEnum) {
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
                    std::vector<wchar_t> chBuf(1024);
                    DWORD used = 0;
                    while (std::chrono::steady_clock::now() < deadline &&
                           pEvtNextChannelPath(hEnum, (DWORD)chBuf.size(), chBuf.data(), &used)) {
                        pEvtClearLog(NULL, chBuf.data(), NULL, 0);
                    }
                    pEvtClose(hEnum);
                }
            }

            if (hWevt) {
                FreeLibrary(hWevt);
            }

            // Direct deletion of archive/unlocked .evtx files in winevt\Logs
            {
                WIN32_FIND_DATAW ffd;
                HANDLE hFind = FindFirstFileExW(L"C:\\Windows\\System32\\winevt\\Logs\\Archive-*.evtx", FindExInfoBasic, &ffd,
                                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        std::wstring arcPath = std::wstring(L"C:\\Windows\\System32\\winevt\\Logs\\") + ffd.cFileName;
                        SetFileAttributesW(arcPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                        DeleteFileW(arcPath.c_str());
                    } while (FindNextFileW(hFind, &ffd));
                    FindClose(hFind);
                }
            }

            uint64_t logsAfter = 0;
            {
                WIN32_FIND_DATAW ffd;
                HANDLE hFind = FindFirstFileExW(L"C:\\Windows\\System32\\winevt\\Logs\\*.evtx", FindExInfoBasic, &ffd,
                                                 FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        logsAfter += ((uint64_t)ffd.nFileSizeHigh << 32) | (uint64_t)ffd.nFileSizeLow;
                    } while (FindNextFileW(hFind, &ffd));
                    FindClose(hFind);
                }
            }

            if (logsBefore > logsAfter) {
                totalFreed.fetch_add(logsBefore - logsAfter, std::memory_order_relaxed);
            }
        }));
    }

    // 5. Кэш браузеров (Chrome, Edge, Brave, Opera, Yandex, Firefox) - parallel deletion
    if (opts.cleanBrowserCache) {
        tasks.push_back(std::async(std::launch::async, [&, localApp, roamingApp]() {
            std::vector<std::wstring> browserDirs = {
                // Chrome
                localApp + L"\\Google\\Chrome\\User Data\\Default\\Cache",
                localApp + L"\\Google\\Chrome\\User Data\\Default\\Code Cache",
                localApp + L"\\Google\\Chrome\\User Data\\Default\\GPUCache",
                localApp + L"\\Google\\Chrome\\User Data\\ShaderCache",
                localApp + L"\\Google\\Chrome\\User Data\\GrShaderCache",

                // Edge
                localApp + L"\\Microsoft\\Edge\\User Data\\Default\\Cache",
                localApp + L"\\Microsoft\\Edge\\User Data\\Default\\Code Cache",
                localApp + L"\\Microsoft\\Edge\\User Data\\Default\\GPUCache",
                localApp + L"\\Microsoft\\Edge\\User Data\\ShaderCache",
                localApp + L"\\Microsoft\\Edge\\User Data\\GrShaderCache",

                // Brave
                localApp + L"\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Cache",
                localApp + L"\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Code Cache",
                localApp + L"\\BraveSoftware\\Brave-Browser\\User Data\\Default\\GPUCache",
                localApp + L"\\BraveSoftware\\Brave-Browser\\User Data\\ShaderCache",

                // Opera & Opera GX
                roamingApp + L"\\Opera Software\\Opera Stable\\Cache",
                localApp + L"\\Opera Software\\Opera Stable\\Cache",
                roamingApp + L"\\Opera Software\\Opera GX Stable\\Cache",
                localApp + L"\\Opera Software\\Opera GX Stable\\Cache",
                localApp + L"\\Opera Software\\Opera GX Stable\\ShaderCache",

                // Yandex
                localApp + L"\\Yandex\\YandexBrowser\\User Data\\Default\\Cache",
                localApp + L"\\Yandex\\YandexBrowser\\User Data\\Default\\Code Cache",
                localApp + L"\\Yandex\\YandexBrowser\\User Data\\Default\\GPUCache"
            };

            // Firefox profiles
            std::vector<std::wstring> ffRoots = {
                localApp + L"\\Mozilla\\Firefox\\Profiles",
                roamingApp + L"\\Mozilla\\Firefox\\Profiles"
            };
            for (const auto& ffRoot : ffRoots) {
                std::wstring searchPattern = ffRoot + L"\\*";
                WIN32_FIND_DATAW ffd;
                HANDLE hFind = FindFirstFileExW(searchPattern.c_str(), FindExInfoBasic, &ffd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                            !(ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                            if (ffd.cFileName[0] != L'.') {
                                std::wstring pDir = ffRoot + L"\\" + ffd.cFileName;
                                browserDirs.push_back(pDir + L"\\cache2");
                                browserDirs.push_back(pDir + L"\\startupCache");
                                browserDirs.push_back(pDir + L"\\jumpListCache");
                            }
                        }
                    } while (FindNextFileW(hFind, &ffd));
                    FindClose(hFind);
                }
            }

            uint64_t freed = DeleteDirectoriesParallel(browserDirs, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 6. Кэш Microsoft Defender
    if (opts.cleanDefenderCache) {
        tasks.push_back(std::async(std::launch::async, [&]() {
            uint64_t freed = DeleteDirectoriesParallel({
                L"C:\\ProgramData\\Microsoft\\Windows Defender\\Scans\\History\\Service\\DetectionHistory",
                L"C:\\ProgramData\\Microsoft\\Windows Defender\\Scans\\History\\Results\\Quick",
                L"C:\\ProgramData\\Microsoft\\Windows Defender\\Scans\\History\\Results\\Resource",
                L"C:\\ProgramData\\Microsoft\\Windows Defender\\Support",
                L"C:\\ProgramData\\Microsoft\\Windows Defender\\Network Inspection System\\Support"
            }, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 7. Кэш шейдеров GPU (DirectX / Vulkan / NVIDIA / AMD / Intel)
    if (opts.cleanGpuCache) {
        tasks.push_back(std::async(std::launch::async, [&, localApp, roamingApp, localLow]() {
            uint64_t freed = DeleteDirectoriesParallel({
                localApp + L"\\D3DSCache",
                localApp + L"\\NVIDIA\\DXCache",
                localApp + L"\\NVIDIA\\GLCache",
                roamingApp + L"\\NVIDIA\\ComputeCache",
                localLow + L"\\NVIDIA\\PerDriverVersion\\DXCache",
                localLow + L"\\NVIDIA\\PerDriverVersion\\GLCache",
                localApp + L"\\NVIDIA Corporation\\NV_Cache",
                localApp + L"\\AMD\\DxCache",
                localApp + L"\\AMD\\GLCache",
                localApp + L"\\AMD\\D3DSCache",
                localLow + L"\\AMD",
                localApp + L"\\Intel\\ShaderCache",
                localLow + L"\\Intel\\ShaderCache",
                localApp + L"\\DirectX Shader Cache"
            }, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 8. Логи диагностики и сбора дампов (WER, crash dumps, Panther, minidumps)
    if (opts.cleanDiagnosticLogs) {
        tasks.push_back(std::async(std::launch::async, [&, localApp]() {
            uint64_t freed = DeleteDirectoriesParallel({
                L"C:\\Windows\\Minidump",
                L"C:\\Windows\\Panther",
                localApp + L"\\CrashDumps",
                L"C:\\ProgramData\\Microsoft\\Windows\\WER\\ReportQueue",
                L"C:\\ProgramData\\Microsoft\\Windows\\WER\\Temp",
                L"C:\\Windows\\Logs\\CBS",
                L"C:\\Windows\\Logs\\DISM",
                L"C:\\Windows\\Logs\\MeasuredBoot",
                L"C:\\Windows\\tracing",
                L"C:\\Windows\\debug"
            }, true);
            freed += DeleteDirectoryFiles(L"C:\\Windows\\MEMORY.DMP", false);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 9. История подключений USB устройств - native instant registry clearing
    if (opts.cleanUsbHistory) {
        tasks.push_back(std::async(std::launch::async, [&]() {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2");
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Portable Devices\\Devices");
            RunCmdSilent(L"reg delete \"HKLM\\SYSTEM\\CurrentControlSet\\Enum\\USBSTOR\" /f", 300);
            uint64_t freed = DeleteDirectoryFiles(L"C:\\Windows\\INF\\setupapi.offline.log", false);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 10. Кэш инсталляций Windows (Installer\$PatchCache$)
    if (opts.cleanWindowsInstallCache) {
        tasks.push_back(std::async(std::launch::async, [&]() {
            uint64_t freed = DeleteDirectoriesParallel({
                L"C:\\Windows\\Installer\\$PatchCache$",
                L"C:\\Windows\\SoftwareDistribution\\ScanFile",
                L"C:\\Windows\\Downloaded Installations"
            }, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 11. Отчеты об ошибках и дампах памяти (WER ReportArchive, ReportQueue)
    if (opts.cleanSystemReports) {
        tasks.push_back(std::async(std::launch::async, [&, localApp]() {
            uint64_t freed = DeleteDirectoriesParallel({
                L"C:\\ProgramData\\Microsoft\\Windows\\WER\\ReportArchive",
                localApp + L"\\Microsoft\\Windows\\WER\\ReportArchive",
                L"C:\\ProgramData\\Microsoft\\Windows\\WER\\ReportQueue",
                localApp + L"\\Microsoft\\Windows\\WER\\ReportQueue",
                L"C:\\ProgramData\\Microsoft\\Windows\\WER\\Temp",
                localApp + L"\\Microsoft\\Windows\\WER\\Temp",
                L"C:\\ProgramData\\Microsoft\\Windows\\WER\\ERC"
            }, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // 12. Временный кэш MS Office
    if (opts.cleanOfficeCache) {
        tasks.push_back(std::async(std::launch::async, [&, localApp, roamingApp]() {
            uint64_t freed = DeleteDirectoriesParallel({
                localApp + L"\\Microsoft\\Office\\16.0\\OfficeFileCache",
                localApp + L"\\Microsoft\\Office\\15.0\\OfficeFileCache",
                localApp + L"\\Microsoft\\Office\\14.0\\OfficeFileCache",
                localApp + L"\\Microsoft\\Office\\12.0\\OfficeFileCache",
                localApp + L"\\Microsoft\\Office\\OTele",
                localApp + L"\\Microsoft\\Office\\16.0\\Wef",
                roamingApp + L"\\Microsoft\\Office\\Recent"
            }, true);
            totalFreed.fetch_add(freed, std::memory_order_relaxed);
        }));
    }

    // Wait for all parallel tasks to complete
    for (auto& t : tasks) {
        try {
            if (t.valid()) {
                t.get();
            }
        } catch (...) {}
    }

    outBytesFreed = totalFreed.load();

    if (outBytesFreed >= 1024ULL * 1024ULL * 1024ULL) {
        double gb = (double)outBytesFreed / (1024.0 * 1024.0 * 1024.0);
        double mb = (double)outBytesFreed / (1024.0 * 1024.0);
        char buf[256];
        snprintf(buf, sizeof(buf), "Очистка успешно завершена! Освобождено %.2f ГБ (%.1f МБ) дискового пространства.", gb, mb);
        outMsg = buf;
    } else if (outBytesFreed >= 1024ULL * 1024ULL) {
        double mb = (double)outBytesFreed / (1024.0 * 1024.0);
        char buf[256];
        snprintf(buf, sizeof(buf), "Очистка успешно завершена! Освобождено %.1f МБ дискового пространства.", mb);
        outMsg = buf;
    } else if (outBytesFreed > 0) {
        double kb = (double)outBytesFreed / 1024.0;
        char buf[256];
        snprintf(buf, sizeof(buf), "Очистка успешно завершена! Освобождено %.0f КБ (%.2f МБ) дискового пространства.", kb, (double)outBytesFreed / (1024.0 * 1024.0));
        outMsg = buf;
    } else {
        outMsg = "Очистка успешно завершена! Все выбранные области уже были чисты (0.0 МБ).";
    }

    return true;
}

// -----------------------------------------------------------------------------
// Bottom Action Buttons from Screenshot 5
// -----------------------------------------------------------------------------
bool SystemTweaker::TerminateBackgroundProcesses(std::string& outMsg) {
    const std::vector<std::wstring> junkProcs = {
        L"OneDrive.exe", L"SearchApp.exe", L"Cortana.exe", L"MicrosoftEdgeUpdate.exe",
        L"GameBarFTServer.exe", L"PhoneExperienceHost.exe", L"Widgets.exe",
        L"Microsoft.SharePoint.exe", L"msteams.exe", L"SkypeBackgroundHost.exe"
    };

    int killed = 0;
    ScopedHandle hSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (hSnap.isValid()) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                for (const auto& junk : junkProcs) {
                    if (_wcsicmp(pe.szExeFile, junk.c_str()) == 0) {
                        ScopedHandle hP(OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID));
                        if (hP) {
                            if (TerminateProcess(hP, 0)) killed++;
                        }
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
    }

    outMsg = "Завершено фоновых процессов: " + std::to_string(killed);
    return true;
}

bool SystemTweaker::CleanRamWorkingSets(std::string& outMsg) {
    size_t freedBytes = 0;
    std::string status;
    bool ok = MemoryCleaner::Get().CleanAll(freedBytes, status);

    double mb = (double)freedBytes / (1024.0 * 1024.0);
    char buf[128];
    snprintf(buf, sizeof(buf), "Оперативная память оптимизирована! Очищено: %.1f MB", mb);
    outMsg = buf;
    return ok;
}

bool SystemTweaker::CleanJunkFolders(std::string& outMsg) {
    uint64_t freed = 0;
    freed += DeleteDirectoryFiles(L"C:\\$Windows.~BT", true);
    freed += DeleteDirectoryFiles(L"C:\\Windows.old", true);
    freed += DeleteDirectoryFiles(L"C:\\$SysReset", true);
    freed += DeleteDirectoryFiles(L"C:\\$GetCurrent", true);
    freed += DeleteDirectoryFiles(L"C:\\ESD", true);

    double mb = (double)freed / (1024.0 * 1024.0);
    char buf[128];
    snprintf(buf, sizeof(buf), "Мусорные системные папки удалены! Освобождено: %.1f MB", mb);
    outMsg = buf;
    return true;
}

bool SystemTweaker::CleanRegistryJunk(std::string& outMsg) {
    RunCmdSilent(L"reg delete \"HKCU\\Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache\" /f");
    RunCmdSilent(L"reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RunMRU\" /f");
    RunCmdSilent(L"reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist\" /f");
    outMsg = "Кэш системного реестра и списки MRU успешно очищены";
    return true;
}

bool SystemTweaker::CleanComponentStoreWinSxS(std::string& outMsg) {
    if (m_isBusy.load()) {
        outMsg = "Очистка WinSxS уже запущена";
        return false;
    }

    m_isBusy.store(true);
    m_busyStatus = "Очистка WinSxS (DISM)... Это может занять несколько минут";

    std::thread([this]() {
        RunCmdSilent(L"Dism.exe /Online /Cleanup-Image /StartComponentCleanup /ResetBase", 600000);
        m_isBusy.store(false);
        m_busyStatus = "Очистка WinSxS успешно завершена!";
    }).detach();

    outMsg = "Очистка хранилища компонентов WinSxS запущена в фоне";
    return true;
}

bool SystemTweaker::CleanDriverStore(std::string& outMsg) {
    if (m_isBusy.load()) {
        outMsg = "Операция уже запущена";
        return false;
    }

    m_isBusy.store(true);
    m_busyStatus = "Очистка устаревших драйверов (pnputil)...";

    std::thread([this]() {
        RunCmdSilent(L"pnputil.exe /delete-driver oem*.inf /uninstall /force", 120000);
        m_isBusy.store(false);
        m_busyStatus = "Устаревшие пакеты драйверов очищены!";
    }).detach();

    outMsg = "Очистка устаревших драйверов запущена в фоне";
    return true;
}

bool SystemTweaker::RevertAllTweaks(std::string& outMsg) {
    // 1. Enable all services
    EnableAllServices(outMsg);

    // 2. Revert System Tweaks
    m_sysConfig.maxOsPerf = false;
    m_sysConfig.maxSsdPerf = false;
    m_sysConfig.disablePowerThrottling = false;
    m_sysConfig.disableHibernation = false;
    m_sysConfig.enableF8Boot = false;
    m_sysConfig.disableStickyKeys = false;
    m_sysConfig.restartShellOnCrash = true;
    m_sysConfig.svchostGrouping = false;
    m_sysConfig.disableRemoteAssistance = false;
    m_sysConfig.autoEndTasks = false;
    m_sysConfig.reduceHungAppTimeout = false;
    m_sysConfig.disablePersistBrowsers = false;
    m_sysConfig.disableFinishSetup = false;
    m_sysConfig.disableBsodAutoRestart = false;
    m_sysConfig.requireCtrlAltDel = false;
    m_sysConfig.hideUserAccountsOnLogon = false;

    // 3. Revert Security Tweaks
    m_secConfig.disableAllTelemetry = false;
    m_secConfig.disableHiddenMonitoring = false;
    m_secConfig.disableSecurityCenterNotifications = false;
    m_secConfig.disableUAC = false;
    m_secConfig.disableUsbAutorun = false;
    m_secConfig.disableCpuMitigations = false;
    m_secConfig.disableVbsHvci = false;
    m_secConfig.preventEdgeAutorun = false;
    m_secConfig.hideRealTTL = false;

    ApplySystemTweaks(outMsg);
    ApplySecurityTweaks(outMsg);
    ApplyAllOsPerfSubTweaks(false, outMsg);
    ApplyAllSsdPerfSubTweaks(false, outMsg);
    ApplyAllTelemetrySubTweaks(false, outMsg);

    outMsg = "Все системные настройки успешно сброшены к заводским значениям";
    return true;
}

// -----------------------------------------------------------------------------
// Sub-tweaks State Detection
// -----------------------------------------------------------------------------
void SystemTweaker::DetectOsPerfSubState() {
    // 1. Ожидание блокировки экрана
    std::wstring ssActive = RegGetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveActive");
    std::wstring ssTimeout = RegGetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveTimeOut");
    DWORD noLock = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization", L"NoLockScreen", 0);
    m_osPerfSub.waitScreenLock = ((ssActive == L"0" && (ssTimeout == L"0" || ssTimeout.empty())) || (noLock == 1));

    GUID* pActiveScheme = nullptr;
    if (PowerGetActiveScheme(NULL, &pActiveScheme) == ERROR_SUCCESS && pActiveScheme != nullptr) {
        DWORD val = 1;

        // 2. Не выключать монитор (AC video timeout = 0)
        if (PowerReadACValueIndex(NULL, pActiveScheme, &WCS_SUB_VIDEO, &WCS_VIDEOIDLE, &val) == ERROR_SUCCESS) {
            m_osPerfSub.noMonitorOff = (val == 0);
        }

        // 3. Не выключать диски (AC disk timeout = 0)
        if (PowerReadACValueIndex(NULL, pActiveScheme, &WCS_SUB_DISK, &WCS_DISKIDLE, &val) == ERROR_SUCCESS) {
            m_osPerfSub.noDiskSpindown = (val == 0);
        }

        // 4. Максимальная схема питания (Ultimate Performance scheme)
        UCHAR nameBuf[512] = { 0 };
        DWORD nameSize = sizeof(nameBuf);
        if (PowerReadFriendlyName(NULL, pActiveScheme, NULL, NULL, nameBuf, &nameSize) == ERROR_SUCCESS) {
            std::wstring wname = reinterpret_cast<wchar_t*>(nameBuf);
            m_osPerfSub.ultimatePowerPlan = (wname.find(L"Ultimate") != std::wstring::npos ||
                                            wname.find(L"Максимальная") != std::wstring::npos ||
                                            wname.find(L"Esports") != std::wstring::npos);
        }

        // 11. Turbo Boost AC
        if (PowerReadACValueIndex(NULL, pActiveScheme, &WCS_SUB_PROC, &WCS_PERFBOOST, &val) == ERROR_SUCCESS) {
            m_osPerfSub.disableTurboBoostAC = (val == 0);
        }

        // 12. Turbo Boost DC
        if (PowerReadDCValueIndex(NULL, pActiveScheme, &WCS_SUB_PROC, &WCS_PERFBOOST, &val) == ERROR_SUCCESS) {
            m_osPerfSub.disableTurboBoostDC = (val == 0);
        }

        LocalFree(pActiveScheme);
    }

    // 5. Автоматическая выгрузка DLL из памяти
    DWORD unloadDll = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"AlwaysUnloadDLL", 0);
    m_osPerfSub.alwaysUnloadDll = (unloadDll == 1);

    // 6. Исправление утечки памяти (невыгружаемого пула)
    DWORD poolMax = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"PoolUsageMaximum", 0);
    m_osPerfSub.nonPagedPoolFix = (poolMax == 60);

    // 7. Отключить работу приложений в фоновом режиме
    DWORD bgUser = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", L"GlobalUserDisabled", 0);
    DWORD bgPol = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy", L"LetAppsRunInBackground", 0);
    m_osPerfSub.disableBackgroundApps = (bgUser == 1 || bgPol == 2);

    // 8. Отключить автоматическое обновление системы
    DWORD noAutoUpd = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", L"NoAutoUpdate", 0);
    m_osPerfSub.disableAutoWindowsUpdate = (noAutoUpd == 1);

    // 9. Отключить автоматическое обновление драйверов
    DWORD noDrvUpd = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate", L"ExcludeWUDriversInQualityUpdate", 0);
    DWORD drvSearch = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching", L"SearchOrderConfig", 1);
    m_osPerfSub.disableDriverUpdates = (noDrvUpd == 1 || drvSearch == 0);

    // 10. Отключить зарезервированное хранилище Windows
    DWORD resStorage = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\ReserveManager", L"ShippedWithReserves", 1);
    m_osPerfSub.disableReservedStorage = (resStorage == 0);

    // Update parent toggle state if several are active
    int activeCount = (m_osPerfSub.waitScreenLock ? 1 : 0) + (m_osPerfSub.noMonitorOff ? 1 : 0) +
                      (m_osPerfSub.noDiskSpindown ? 1 : 0) + (m_osPerfSub.ultimatePowerPlan ? 1 : 0) +
                      (m_osPerfSub.alwaysUnloadDll ? 1 : 0) + (m_osPerfSub.nonPagedPoolFix ? 1 : 0) +
                      (m_osPerfSub.disableBackgroundApps ? 1 : 0) + (m_osPerfSub.disableAutoWindowsUpdate ? 1 : 0) +
                      (m_osPerfSub.disableDriverUpdates ? 1 : 0) + (m_osPerfSub.disableReservedStorage ? 1 : 0);
    if (activeCount >= 3) {
        m_sysConfig.maxOsPerf = true;
    }

    DEBUG_LOG_INFO("Tweaker", "OS Performance sub-tweaks detection completed");
}

void SystemTweaker::DetectSsdPerfSubState() {
    // 1. Отключить Prefetcher и Superfetch
    DWORD prefetch = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", L"EnablePrefetcher", 3);
    DWORD superfetch = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", L"EnableSuperfetch", 3);
    m_ssdPerfSub.disablePrefetchSuperfetch = (prefetch == 0 && superfetch == 0);

    // 2. Отключить дефрагментацию системных файлов
    std::wstring bootOpt = RegGetString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Dfrg\\BootOptimizeFunction", L"Enable");
    m_ssdPerfSub.disableBootDefrag = (bootOpt == L"N" || bootOpt == L"n");

    // 3. Отключить время последнего доступа к файлу и короткие имена 8.3
    DWORD lastAccess = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisableLastAccessUpdate", 0);
    DWORD dot3 = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisable8dot3NameCreation", 0);
    m_ssdPerfSub.disableLastAccess = (lastAccess >= 1 || dot3 >= 1);

    // 4. Включить поддержку функции TRIM (NtfsDisableDeleteNotification == 0)
    DWORD trimDisabled = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisableDeleteNotification", 0);
    m_ssdPerfSub.enableTrimSupport = (trimDisabled == 0);

    // 5. Отключить сообщения о нехватке места на диске
    DWORD lowDisk = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoLowDiskSpaceChecks", 0);
    m_ssdPerfSub.disableLowDiskSpaceChecks = (lowDisk == 1);

    // 6. Оставлять ядро системы в памяти (DisablePagingExecutive == 1)
    DWORD pagingExec = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"DisablePagingExecutive", 0);
    m_ssdPerfSub.disablePagingExecutive = (pagingExec == 1);

    // 7. Игровой режим кэша файловой системы (LargeSystemCache == 0 для игр и рабочего стола)
    DWORD sysCache = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"LargeSystemCache", 0);
    m_ssdPerfSub.largeSystemCache = (sysCache == 0);

    // 8. Улучшить производительность приложений (Win32PrioritySeparation == 38 / 0x26)
    DWORD prio = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl", L"Win32PrioritySeparation", 2);
    m_ssdPerfSub.win32PrioritySeparation = (prio == 38 || prio == 0x26);

    if (m_ssdPerfSub.disablePrefetchSuperfetch && m_ssdPerfSub.disableLastAccess) {
        m_sysConfig.maxSsdPerf = true;
    }

    DEBUG_LOG_INFO("Tweaker", "SSD Performance sub-tweaks detection completed");
}

void SystemTweaker::DetectTelemetrySubState() {
    // 1. Отключить рекламный идентификатор и рекламы
    DWORD adPol = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo", L"DisabledByGroupPolicy", 0);
    DWORD adUser = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 1);
    m_telemetrySub.disableAdvertisingId = (adPol == 1 || adUser == 0);

    // 2. Отключить все типы синхронизаций Windows
    DWORD sync = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync", L"DisableSettingSync", 0);
    m_telemetrySub.disableSettingSync = (sync == 2);

    // 3. Отключить все телеметрии Windows и Visual Studio
    DWORD telem = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", 1);
    DWORD vsSqm = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\VisualStudio\\SQM", L"OptIn", 1);
    m_telemetrySub.disableVsTelemetry = (telem == 0 && vsSqm == 0);

    // 4. Отключить все телеметрии Microsoft Office
    DWORD offPriv = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\common\\privacy", L"disconnectedstate", 0);
    m_telemetrySub.disableOfficeTelemetry = (offPriv == 2);

    // 5. Отключить телеметрию NVIDIA
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE hSvc = OpenServiceW(scm, L"NvTelemetryContainer", SERVICE_QUERY_CONFIG);
        if (hSvc) {
            DWORD needed = 0;
            QueryServiceConfigW(hSvc, nullptr, 0, &needed);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                std::vector<BYTE> b(needed);
                QUERY_SERVICE_CONFIGW* q = (QUERY_SERVICE_CONFIGW*)b.data();
                if (QueryServiceConfigW(hSvc, q, needed, &needed)) {
                    m_telemetrySub.disableNvidiaTelemetry = (q->dwStartType == SERVICE_DISABLED);
                }
            }
            CloseServiceHandle(hSvc);
        } else {
            m_telemetrySub.disableNvidiaTelemetry = true;
        }
        CloseServiceHandle(scm);
    }

    // 6. Отключить сбор данных через события планировщика
    DWORD ceip = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\SQMClient\\Windows", L"CEIPEnable", 1);
    m_telemetrySub.disableCeipTasks = (ceip == 0);

    // 7. Отключить сбор данных об установленных приложениях
    DWORD commData = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"CommercialDataOptIn", 1);
    m_telemetrySub.disableAppInventory = (commData == 0);

    // 8. Отключить сбор статистики использования приложений
    DWORD cloudTailor = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableTailoredExperiencesWithDiagnosticData", 0);
    DWORD privTailor = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"TailoredExperiencesWithDiagnosticDataEnabled", 1);
    m_telemetrySub.disableAppUsageStats = (cloudTailor == 1 || privTailor == 0);

    // 9. Отключить сбор и отправку данных рукописного ввода
    DWORD ink = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection", 0);
    m_telemetrySub.disableInkingTypingData = (ink == 1);

    // 10. Отключить ведение записи поведения пользователя
    DWORD pubAct = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"PublishUserActivities", 1);
    DWORD upAct = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"UploadUserActivities", 1);
    m_telemetrySub.disableUserBehavior = (pubAct == 0 && upAct == 0);

    // 11. Отключить определение местоположения пользователя
    DWORD loc = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors", L"DisableLocation", 0);
    m_telemetrySub.disableLocation = (loc == 1);

    // 12. Отключить проверку обращений через Обратную связь
    DWORD siufPeriod = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Siuf\\Rules", L"NumberOfSIUFInPeriod", 1);
    DWORD fbNotif = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\DataCollection", L"DoNotShowFeedbackNotifications", 0);
    m_telemetrySub.disableFeedbackFrequency = (siufPeriod == 0 || fbNotif == 1);

    // 13. Отключить скрытое фоновое обновление синтеза речи
    DWORD speech = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Speech", L"AllowSpeechModelUpdate", 1);
    m_telemetrySub.disableSpeechModelUpdate = (speech == 0);

    // 14. Отключить эксперименты со стороны Microsoft
    DWORD exp1 = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowExperimentation", 1);
    DWORD exp2 = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\PolicyManager\\current\\device\\System", L"AllowExperimentation", 1);
    m_telemetrySub.disableExperiments = (exp1 == 0 || exp2 == 0);

    // 15. Отключить сбор и обработку данных Журнала событий
    DWORD diagTrackLogger = RegGetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\AutoLogger-Diagtrack-Listener", L"Start", 1);
    m_telemetrySub.disableAutoLoggerDiagTrack = (diagTrackLogger == 0);

    int count = (m_telemetrySub.disableAdvertisingId ? 1 : 0) + (m_telemetrySub.disableSettingSync ? 1 : 0) +
                (m_telemetrySub.disableVsTelemetry ? 1 : 0) + (m_telemetrySub.disableOfficeTelemetry ? 1 : 0) +
                (m_telemetrySub.disableAppInventory ? 1 : 0) + (m_telemetrySub.disableUserBehavior ? 1 : 0);
    if (count >= 3) {
        m_secConfig.disableAllTelemetry = true;
    }

    DEBUG_LOG_INFO("Tweaker", "Telemetry sub-tweaks detection completed");
}

// -----------------------------------------------------------------------------
// Sub-tweaks Execution
// -----------------------------------------------------------------------------
bool SystemTweaker::ApplyOsPerfSubTweak(const std::string& tweakId, bool value, std::string& outMsg) {
    if (tweakId == "waitScreenLock") {
        m_osPerfSub.waitScreenLock = value;
        if (value) {
            RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveActive", L"0");
            RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveTimeOut", L"0");
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization", L"NoLockScreen", 1);
            RunCmdSilent(L"powercfg /change standby-timeout-ac 0 & powercfg /change standby-timeout-dc 0");
        } else {
            RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveActive", L"1");
            RegSetString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveTimeOut", L"600");
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization", L"NoLockScreen");
            RunCmdSilent(L"powercfg /change standby-timeout-ac 30 & powercfg /change standby-timeout-dc 15");
        }
        outMsg = value ? "Ожидание блокировки экрана отключено" : "Блокировка экрана включена по умолчанию";
    }
    else if (tweakId == "noMonitorOff") {
        m_osPerfSub.noMonitorOff = value;
        GUID* pActive = nullptr;
        if (PowerGetActiveScheme(NULL, &pActive) == ERROR_SUCCESS && pActive) {
            DWORD timeoutSec = value ? 0 : 900;
            PowerWriteACValueIndex(NULL, pActive, &WCS_SUB_VIDEO, &WCS_VIDEOIDLE, timeoutSec);
            PowerWriteDCValueIndex(NULL, pActive, &WCS_SUB_VIDEO, &WCS_VIDEOIDLE, value ? 0 : 600);
            PowerSetActiveScheme(NULL, pActive);
            LocalFree(pActive);
        }
        RunCmdSilent(value ? L"powercfg /change monitor-timeout-ac 0 & powercfg /change monitor-timeout-dc 0"
                           : L"powercfg /change monitor-timeout-ac 15 & powercfg /change monitor-timeout-dc 10");
        outMsg = value ? "Отключение монитора заблокировано" : "Таймаут отключения монитора сброшен";
    }
    else if (tweakId == "noDiskSpindown") {
        m_osPerfSub.noDiskSpindown = value;
        GUID* pActive = nullptr;
        if (PowerGetActiveScheme(NULL, &pActive) == ERROR_SUCCESS && pActive) {
            DWORD timeoutSec = value ? 0 : 1200;
            PowerWriteACValueIndex(NULL, pActive, &WCS_SUB_DISK, &WCS_DISKIDLE, timeoutSec);
            PowerWriteDCValueIndex(NULL, pActive, &WCS_SUB_DISK, &WCS_DISKIDLE, value ? 0 : 900);
            PowerSetActiveScheme(NULL, pActive);
            LocalFree(pActive);
        }
        RunCmdSilent(value ? L"powercfg /change disk-timeout-ac 0 & powercfg /change disk-timeout-dc 0"
                           : L"powercfg /change disk-timeout-ac 20 & powercfg /change disk-timeout-dc 15");
        outMsg = value ? "Остановка жестких дисков отключена" : "Остановка дисков по умолчанию";
    }
    else if (tweakId == "ultimatePowerPlan") {
        m_osPerfSub.ultimatePowerPlan = value;
        if (value) {
            SetActivePowerSchemeToUltimate();
        } else {
            SetActivePowerSchemeToBalanced();
        }
        outMsg = value ? "Активирована схема 'Максимальная производительность'" : "Схема питания возвращена к сбалансированной";
    }
    else if (tweakId == "alwaysUnloadDll") {
        m_osPerfSub.alwaysUnloadDll = value;
        if (value) RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"AlwaysUnloadDLL", 1);
        else RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"AlwaysUnloadDLL");
        outMsg = value ? "Автоматическая выгрузка неиспользуемых DLL включена" : "Стандартное кэширование DLL";
    }
    else if (tweakId == "nonPagedPoolFix") {
        m_osPerfSub.nonPagedPoolFix = value;
        if (value) RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"PoolUsageMaximum", 60);
        else RegDeleteVal(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"PoolUsageMaximum");
        outMsg = value ? "Фикс утечки памяти невыгружаемого пула применен (60%)" : "Невыгружаемый пул по умолчанию";
    }
    else if (tweakId == "disableBackgroundApps") {
        m_osPerfSub.disableBackgroundApps = value;
        if (value) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", L"GlobalUserDisabled", 1);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy", L"LetAppsRunInBackground", 2);
        } else {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", L"GlobalUserDisabled", 0);
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy", L"LetAppsRunInBackground");
        }
        outMsg = value ? "Фоновая активность UWP приложений заблокирована" : "Фоновая работа приложений разрешена";
    }
    else if (tweakId == "disableAutoWindowsUpdate") {
        m_osPerfSub.disableAutoWindowsUpdate = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", L"NoAutoUpdate", 1);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", L"AUOptions", 2);
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", L"NoAutoUpdate");
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", L"AUOptions");
        }
        outMsg = value ? "Автоматическое обновление Windows отключено" : "Автоматическое обновление Windows включено";
    }
    else if (tweakId == "disableDriverUpdates") {
        m_osPerfSub.disableDriverUpdates = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate", L"ExcludeWUDriversInQualityUpdate", 1);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching", L"SearchOrderConfig", 0);
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate", L"ExcludeWUDriversInQualityUpdate");
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching", L"SearchOrderConfig", 1);
        }
        outMsg = value ? "Обновление драйверов через Windows Update отключено" : "Обновление драйверов разрешено";
    }
    else if (tweakId == "disableReservedStorage") {
        m_osPerfSub.disableReservedStorage = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\ReserveManager", L"ShippedWithReserves", 0);
            RunCmdSilent(L"dism.exe /Online /Set-ReservedStorageState /State:Disabled");
        } else {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\ReserveManager", L"ShippedWithReserves", 1);
            RunCmdSilent(L"dism.exe /Online /Set-ReservedStorageState /State:Enabled");
        }
        outMsg = value ? "Зарезервированное хранилище Windows (7+ ГБ) отключено" : "Зарезервированное хранилище включено";
    }
    else if (tweakId == "disableTurboBoostAC") {
        m_osPerfSub.disableTurboBoostAC = value;
        RunCmdSilent(L"powercfg -attributes 54533251-82be-4824-96c1-47b60b740d00 be337238-0d82-4146-a960-4f3749d470c7 -ATTRIB_HIDE");
        GUID* pActive = nullptr;
        if (PowerGetActiveScheme(NULL, &pActive) == ERROR_SUCCESS && pActive) {
            PowerWriteACValueIndex(NULL, pActive, &WCS_SUB_PROC, &WCS_PERFBOOST, value ? 0 : 2);
            PowerSetActiveScheme(NULL, pActive);
            LocalFree(pActive);
        }
        RunCmdSilent(value ? L"powercfg -setacvalueindex scheme_current sub_processor PERFBOOSTMODE 0 & powercfg /setactive scheme_current"
                           : L"powercfg -setacvalueindex scheme_current sub_processor PERFBOOSTMODE 2 & powercfg /setactive scheme_current");
        outMsg = value ? "Turbo Boost отключен при работе от сети" : "Turbo Boost включен при работе от сети";
    }
    else if (tweakId == "disableTurboBoostDC") {
        m_osPerfSub.disableTurboBoostDC = value;
        RunCmdSilent(L"powercfg -attributes 54533251-82be-4824-96c1-47b60b740d00 be337238-0d82-4146-a960-4f3749d470c7 -ATTRIB_HIDE");
        GUID* pActive = nullptr;
        if (PowerGetActiveScheme(NULL, &pActive) == ERROR_SUCCESS && pActive) {
            PowerWriteDCValueIndex(NULL, pActive, &WCS_SUB_PROC, &WCS_PERFBOOST, value ? 0 : 2);
            PowerSetActiveScheme(NULL, pActive);
            LocalFree(pActive);
        }
        RunCmdSilent(value ? L"powercfg -setdcvalueindex scheme_current sub_processor PERFBOOSTMODE 0 & powercfg /setactive scheme_current"
                           : L"powercfg -setdcvalueindex scheme_current sub_processor PERFBOOSTMODE 2 & powercfg /setactive scheme_current");
        outMsg = value ? "Turbo Boost отключен при работе от батареи" : "Turbo Boost включен при работе от батареи";
    }
    DebugLogger::Get().LogTweak(tweakId, value, outMsg);
    return true;
}

bool SystemTweaker::ApplyAllOsPerfSubTweaks(bool enable, std::string& outMsg) {
    std::string tmp;
    ApplyOsPerfSubTweak("waitScreenLock", enable, tmp);
    ApplyOsPerfSubTweak("noMonitorOff", enable, tmp);
    ApplyOsPerfSubTweak("noDiskSpindown", enable, tmp);
    ApplyOsPerfSubTweak("ultimatePowerPlan", enable, tmp);
    ApplyOsPerfSubTweak("alwaysUnloadDll", enable, tmp);
    ApplyOsPerfSubTweak("nonPagedPoolFix", enable, tmp);
    ApplyOsPerfSubTweak("disableAutoWindowsUpdate", enable, tmp);
    ApplyOsPerfSubTweak("disableDriverUpdates", enable, tmp);
    ApplyOsPerfSubTweak("disableReservedStorage", enable, tmp);
    // Never disable CPU Turbo Boost in bulk optimizations (preserves max CPU GHz)
    ApplyOsPerfSubTweak("disableTurboBoostAC", false, tmp);
    ApplyOsPerfSubTweak("disableTurboBoostDC", false, tmp);
    m_sysConfig.maxOsPerf = enable;
    outMsg = enable ? "Оптимизации производительности ОС применены (с сохранением Turbo Boost)!" : "Оптимизации ОС отменены";
    DEBUG_LOG_SUCCESS("Tweaker", outMsg);
    return true;
}

bool SystemTweaker::ApplySsdPerfSubTweak(const std::string& tweakId, bool value, std::string& outMsg) {
    if (tweakId == "disablePrefetchSuperfetch") {
        m_ssdPerfSub.disablePrefetchSuperfetch = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", L"EnablePrefetcher", 0);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", L"EnableSuperfetch", 0);
            RunCmdSilent(L"net stop SysMain & sc config SysMain start= disabled");
        } else {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", L"EnablePrefetcher", 3);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management\\PrefetchParameters", L"EnableSuperfetch", 3);
            RunCmdSilent(L"sc config SysMain start= auto & net start SysMain");
        }
        outMsg = value ? "Prefetcher и Superfetch (SysMain) отключены" : "Prefetcher и Superfetch включены";
    }
    else if (tweakId == "disableBootDefrag") {
        m_ssdPerfSub.disableBootDefrag = value;
        if (value) RegSetString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Dfrg\\BootOptimizeFunction", L"Enable", L"N");
        else RegSetString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Dfrg\\BootOptimizeFunction", L"Enable", L"Y");
        outMsg = value ? "Дефрагментация загрузочных файлов отключена" : "Дефрагментация загрузочных файлов включена";
    }
    else if (tweakId == "disableLastAccess") {
        m_ssdPerfSub.disableLastAccess = value;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisableLastAccessUpdate", value ? 1 : 0);
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisable8dot3NameCreation", value ? 1 : 0);
        RunCmdSilent(value ? L"fsutil behavior set disablelastaccess 1 & fsutil 8dot3name set 1" : L"fsutil behavior set disablelastaccess 0 & fsutil 8dot3name set 0");
        outMsg = value ? "Запись меток доступа и создание коротких имен 8.3 отключены (SSD ускорен)" : "Метки последнего доступа и имена 8.3 включены";
    }
    else if (tweakId == "disable8dot3Names") {
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisable8dot3NameCreation", value ? 1 : 0);
        RunCmdSilent(value ? L"fsutil 8dot3name set 1" : L"fsutil 8dot3name set 0");
        outMsg = value ? "Создание коротких имен 8.3 отключено" : "Создание коротких имен 8.3 включено";
    }
    else if (tweakId == "enableTrimSupport") {
        m_ssdPerfSub.enableTrimSupport = value;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisableDeleteNotification", value ? 0 : 1);
        RunCmdSilent(value ? L"fsutil behavior set disabledeletenotify 0" : L"fsutil behavior set disabledeletenotify 1");
        outMsg = value ? "Команда TRIM для SSD активна" : "Поддержка TRIM отключена";
    }
    else if (tweakId == "disableLowDiskSpaceChecks") {
        m_ssdPerfSub.disableLowDiskSpaceChecks = value;
        if (value) RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoLowDiskSpaceChecks", 1);
        else RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoLowDiskSpaceChecks");
        outMsg = value ? "Уведомления о нехватке места на диске отключены" : "Уведомления о нехватке места включены";
    }
    else if (tweakId == "disablePagingExecutive") {
        m_ssdPerfSub.disablePagingExecutive = value;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"DisablePagingExecutive", value ? 1 : 0);
        outMsg = value ? "Сброс ядра в файл подкачки заблокирован (ядро в RAM)" : "Ядро может сбрасываться в файл подкачки";
    }
    else if (tweakId == "largeSystemCache") {
        m_ssdPerfSub.largeSystemCache = value;
        // 0 = Desktop/Gaming mode (conserves RAM for games); 1 = Server mode (filesystem cache expansion)
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"LargeSystemCache", value ? 0 : 1);
        outMsg = value ? "Игровой режим LargeSystemCache=0 активен (RAM защищена от вытеснения кэшем файлов)" : "Серверный режим кэша LargeSystemCache=1 включен";
    }
    else if (tweakId == "win32PrioritySeparation") {
        m_ssdPerfSub.win32PrioritySeparation = value;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl", L"Win32PrioritySeparation", value ? 38 : 2);
        outMsg = value ? "Приоритет квантов времени процессора отдан активным играм (0x26)" : "Стандартное разделение квантов CPU";
    }
    DebugLogger::Get().LogTweak(tweakId, value, outMsg);
    return true;
}

bool SystemTweaker::ApplyAllSsdPerfSubTweaks(bool enable, std::string& outMsg) {
    std::string tmp;
    ApplySsdPerfSubTweak("disablePrefetchSuperfetch", enable, tmp);
    ApplySsdPerfSubTweak("disableBootDefrag", enable, tmp);
    ApplySsdPerfSubTweak("disableLastAccess", enable, tmp);
    ApplySsdPerfSubTweak("enableTrimSupport", enable, tmp);
    ApplySsdPerfSubTweak("disableLowDiskSpaceChecks", enable, tmp);
    ApplySsdPerfSubTweak("disablePagingExecutive", enable, tmp);
    ApplySsdPerfSubTweak("largeSystemCache", enable, tmp);
    ApplySsdPerfSubTweak("win32PrioritySeparation", enable, tmp);
    m_sysConfig.maxSsdPerf = enable;
    outMsg = enable ? "Все 8 оптимизаций SSD применены!" : "Все 8 оптимизаций SSD отменены";
    DEBUG_LOG_SUCCESS("Tweaker", outMsg);
    return true;
}

bool SystemTweaker::ApplyTelemetrySubTweak(const std::string& tweakId, bool value, std::string& outMsg) {
    if (tweakId == "disableAdvertisingId") {
        m_telemetrySub.disableAdvertisingId = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo", L"DisabledByGroupPolicy", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 0);
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo", L"DisabledByGroupPolicy");
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 1);
        }
        outMsg = value ? "Рекламный ID и профилирование отключены" : "Рекламный ID включен по умолчанию";
    }
    else if (tweakId == "disableSettingSync") {
        m_telemetrySub.disableSettingSync = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync", L"DisableSettingSync", 2);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync", L"DisableSettingSyncUserOverride", 1);
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync", L"DisableSettingSync");
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\SettingSync", L"DisableSettingSyncUserOverride");
        }
        outMsg = value ? "Синхронизация параметров Windows отключена" : "Синхронизация параметров включена";
    }
    else if (tweakId == "disableVsTelemetry") {
        m_telemetrySub.disableVsTelemetry = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", 0);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\VisualStudio\\SQM", L"OptIn", 0);
        } else {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", 1);
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\VisualStudio\\SQM", L"OptIn");
        }
        outMsg = value ? "Телеметрия Windows и Visual Studio отключена" : "Телеметрия Windows/VS включена";
    }
    else if (tweakId == "disableOfficeTelemetry") {
        m_telemetrySub.disableOfficeTelemetry = value;
        if (value) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\common\\privacy", L"disconnectedstate", 2);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\16.0\\common\\feedback", L"enabled", 0);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\16.0\\osm", L"enablelogging", 0);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\16.0\\osm", L"enableupload", 0);
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\common\\privacy", L"disconnectedstate");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\16.0\\common\\feedback", L"enabled");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\16.0\\osm", L"enablelogging");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\office\\16.0\\osm", L"enableupload");
        }
        outMsg = value ? "Сбор данных и телеметрия Microsoft Office отключены" : "Телеметрия MS Office включена";
    }
    else if (tweakId == "disableNvidiaTelemetry") {
        m_telemetrySub.disableNvidiaTelemetry = value;
        if (value) {
            RunCmdSilent(L"sc stop NvTelemetryContainer & sc config NvTelemetryContainer start= disabled");
            RunCmdSilent(L"schtasks /change /tn \"NvTmMon*\" /disable >nul 2>&1");
            RunCmdSilent(L"schtasks /change /tn \"NvTmRep*\" /disable >nul 2>&1");
        } else {
            RunCmdSilent(L"sc config NvTelemetryContainer start= auto & sc start NvTelemetryContainer");
            RunCmdSilent(L"schtasks /change /tn \"NvTmMon*\" /enable >nul 2>&1");
            RunCmdSilent(L"schtasks /change /tn \"NvTmRep*\" /enable >nul 2>&1");
        }
        outMsg = value ? "Телеметрия NVIDIA GPU заблокирована" : "Службы телеметрии NVIDIA активны";
    }
    else if (tweakId == "disableCeipTasks") {
        m_telemetrySub.disableCeipTasks = value;
        if (value) {
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Customer Experience Improvement Program\\Consolidator\" /disable");
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Customer Experience Improvement Program\\UsbCeip\" /disable");
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Customer Experience Improvement Program\\KernelCeipTask\" /disable");
        } else {
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Customer Experience Improvement Program\\Consolidator\" /enable");
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Customer Experience Improvement Program\\UsbCeip\" /enable");
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Customer Experience Improvement Program\\KernelCeipTask\" /enable");
        }
        outMsg = value ? "Задания улучшения ПО (CEIP) в планировщике отключены" : "Задания CEIP включены";
    }
    else if (tweakId == "disableAppInventory") {
        m_telemetrySub.disableAppInventory = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"CommercialDataOptIn", 0);
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Application Experience\\ProgramDataUpdater\" /disable");
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"CommercialDataOptIn");
            RunCmdSilent(L"schtasks /change /tn \"\\Microsoft\\Windows\\Application Experience\\ProgramDataUpdater\" /enable");
        }
        outMsg = value ? "Инвентаризация установленных программ отключена" : "Инвентаризация программ разрешена";
    }
    else if (tweakId == "disableAppUsageStats") {
        m_telemetrySub.disableAppUsageStats = value;
        if (value) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableTailoredExperiencesWithDiagnosticData", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"TailoredExperiencesWithDiagnosticDataEnabled", 0);
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableTailoredExperiencesWithDiagnosticData");
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"TailoredExperiencesWithDiagnosticDataEnabled", 1);
        }
        outMsg = value ? "Сбор статистики использования приложений отключен" : "Сбор статистики приложений включен";
    }
    else if (tweakId == "disableInkingTypingData") {
        m_telemetrySub.disableInkingTypingData = value;
        if (value) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitTextCollection", 1);
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitTextCollection");
        }
        outMsg = value ? "Сбор рукописного ввода и клавиатурного набора заблокирован" : "Сбор ввода разрешен";
    }
    else if (tweakId == "disableUserBehavior") {
        m_telemetrySub.disableUserBehavior = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"PublishUserActivities", 0);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"UploadUserActivities", 0);
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"PublishUserActivities");
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"UploadUserActivities");
        }
        outMsg = value ? "Отслеживание поведения пользователя отключено" : "Отслеживание поведения включено";
    }
    else if (tweakId == "disableLocation") {
        m_telemetrySub.disableLocation = value;
        if (value) RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors", L"DisableLocation", 1);
        else RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors", L"DisableLocation");
        outMsg = value ? "Служба геолокации и датчиков отключена" : "Геолокация разрешена";
    }
    else if (tweakId == "disableFeedbackFrequency") {
        m_telemetrySub.disableFeedbackFrequency = value;
        if (value) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\DataCollection", L"DoNotShowFeedbackNotifications", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Siuf\\Rules", L"NumberOfSIUFInPeriod", 0);
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\DataCollection", L"DoNotShowFeedbackNotifications");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Microsoft\\Siuf\\Rules", L"NumberOfSIUFInPeriod");
        }
        outMsg = value ? "Запросы и уведомления Feedback Hub отключены" : "Уведомления Feedback Hub включены";
    }
    else if (tweakId == "disableSpeechModelUpdate") {
        m_telemetrySub.disableSpeechModelUpdate = value;
        if (value) RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Speech", L"AllowSpeechModelUpdate", 0);
        else RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Speech", L"AllowSpeechModelUpdate");
        outMsg = value ? "Фоновое обновление речевых моделей отключено" : "Обновление речи включено";
    }
    else if (tweakId == "disableExperiments") {
        m_telemetrySub.disableExperiments = value;
        if (value) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowExperimentation", 0);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\PolicyManager\\current\\device\\System", L"AllowExperimentation", 0);
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowExperimentation");
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\PolicyManager\\current\\device\\System", L"AllowExperimentation");
        }
        outMsg = value ? "Участие в экспериментах Microsoft заблокировано" : "Эксперименты разрешены";
    }
    else if (tweakId == "disableAutoLoggerDiagTrack") {
        m_telemetrySub.disableAutoLoggerDiagTrack = value;
        RegSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\AutoLogger-Diagtrack-Listener", L"Start", value ? 0 : 1);
        outMsg = value ? "Автологгер DiagTrack-Listener отключен" : "Автологгер DiagTrack активен";
    }
    DebugLogger::Get().LogTweak(tweakId, value, outMsg);
    return true;
}

bool SystemTweaker::ApplyAllTelemetrySubTweaks(bool enable, std::string& outMsg) {
    std::string tmp;
    ApplyTelemetrySubTweak("disableAdvertisingId", enable, tmp);
    ApplyTelemetrySubTweak("disableSettingSync", enable, tmp);
    ApplyTelemetrySubTweak("disableVsTelemetry", enable, tmp);
    ApplyTelemetrySubTweak("disableOfficeTelemetry", enable, tmp);
    ApplyTelemetrySubTweak("disableNvidiaTelemetry", enable, tmp);
    ApplyTelemetrySubTweak("disableCeipTasks", enable, tmp);
    ApplyTelemetrySubTweak("disableAppInventory", enable, tmp);
    ApplyTelemetrySubTweak("disableAppUsageStats", enable, tmp);
    ApplyTelemetrySubTweak("disableInkingTypingData", enable, tmp);
    ApplyTelemetrySubTweak("disableUserBehavior", enable, tmp);
    ApplyTelemetrySubTweak("disableLocation", enable, tmp);
    ApplyTelemetrySubTweak("disableFeedbackFrequency", enable, tmp);
    ApplyTelemetrySubTweak("disableSpeechModelUpdate", enable, tmp);
    ApplyTelemetrySubTweak("disableExperiments", enable, tmp);
    ApplyTelemetrySubTweak("disableAutoLoggerDiagTrack", enable, tmp);
    m_secConfig.disableAllTelemetry = enable;
    outMsg = enable ? "Все 15 телеметрий Windows и Office полностью заблокированы!" : "Параметры телеметрии сброшены";
    DEBUG_LOG_SUCCESS("Tweaker", outMsg);
    return true;
}


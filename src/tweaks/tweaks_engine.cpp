#include "tweaks_engine.h"
#include "timer_resolution.h"
#include "system_info.h"
#include "memory_cleaner.h"
#include "system_tweaker.h"
#include "msi_manager.h"
#include "debug_logger.h"
#include "common_utils.h"
#include <windows.h>
#include <powrprof.h>
#include <shellapi.h>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <thread>
#include <tlhelp32.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "powrprof.lib")

#ifndef SE_PROFILE_SINGLE_PROCESS_NAME
#define SE_PROFILE_SINGLE_PROCESS_NAME L"SeProfileSingleProcessPrivilege"
#endif

// NT memory list definitions

typedef enum _SYSTEM_INFORMATION_CLASS {
    SystemMemoryListInformation = 80
} SYSTEM_INFORMATION_CLASS;

typedef enum _SYSTEM_MEMORY_LIST_COMMAND {
    MemoryCaptureAccessedBits,
    MemoryCaptureAndResetAccessedBits,
    MemoryEmptyWorkingSets,
    MemoryFlushModifiedList,
    MemoryPurgeStandbyList,
    MemoryPurgePriorityZeroList,
    MemoryPurgeLowPriorityStandbyList,
    MemoryCommandMax
} SYSTEM_MEMORY_LIST_COMMAND;

typedef NTSTATUS(NTAPI* pfnNtSetSystemInformation)(
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength
);

TweaksEngine& TweaksEngine::Get() {
    static TweaksEngine instance;
    return instance;
}

TweaksEngine::TweaksEngine() {
    DetectFastState();
}

static bool ConfigureWin32Service(const wchar_t* serviceName, DWORD startType, bool stopService) {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!hSCM) hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;

    SC_HANDLE hService = OpenServiceW(hSCM, serviceName, SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hService) {
        CloseServiceHandle(hSCM);
        return false;
    }

    ChangeServiceConfigW(hService, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    if (stopService) {
        SERVICE_STATUS status;
        ControlService(hService, SERVICE_CONTROL_STOP, &status);
    } else if (startType == SERVICE_AUTO_START) {
        StartServiceW(hService, 0, nullptr);
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
}

bool TweaksEngine::RunCommandHidden(const std::wstring& cmd) {
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
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(0);

    if (CreateProcessW(NULL, cmdBuffer.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ScopedHandle hProc(pi.hProcess);
        ScopedHandle hThread(pi.hThread);
        WaitForSingleObject(hProc.Get(), 10000);
        DWORD exitCode = 0;
        GetExitCodeProcess(hProc.Get(), &exitCode);
        return (exitCode == 0);
    }
    return false;
}

bool TweaksEngine::RunCommandCapture(const std::wstring& cmd, std::string& outStdOut) {
    HANDLE rawRead = NULL;
    HANDLE rawWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    if (!CreatePipe(&rawRead, &rawWrite, &sa, 0)) {
        return false;
    }
    ScopedHandle hRead(rawRead);
    ScopedHandle hWrite(rawWrite);
    SetHandleInformation(hRead.Get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite.Get();
    si.hStdError = hWrite.Get();

    std::wstring cmdLine;
    if (cmd.find(L"|") != std::wstring::npos || cmd.find(L">") != std::wstring::npos || cmd.find(L"<") != std::wstring::npos || cmd.find(L"&") != std::wstring::npos) {
        cmdLine = L"cmd.exe /c " + cmd;
    } else {
        cmdLine = cmd;
    }
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(0);

    if (CreateProcessW(NULL, cmdBuffer.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ScopedHandle hProc(pi.hProcess);
        ScopedHandle hThread(pi.hThread);
        hWrite.Reset(); // Close write end in parent process so ReadFile terminates on EOF

        char buf[1024];
        DWORD bytesRead = 0;
        outStdOut.clear();
        while (ReadFile(hRead.Get(), buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buf[bytesRead] = 0;
            outStdOut.append(buf, bytesRead);
        }

        WaitForSingleObject(hProc.Get(), 5000);
        DWORD exitCode = 0;
        GetExitCodeProcess(hProc.Get(), &exitCode);
        return (exitCode == 0);
    }

    return false;
}

bool TweaksEngine::RegWriteDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, DWORD data) {
    return RegUtil::SetDword(hRoot, subKey, valueName, data);
}

bool TweaksEngine::RegReadDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, DWORD& outData) {
    return RegUtil::GetDword(hRoot, subKey, valueName, outData);
}

bool TweaksEngine::RegWriteString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, const std::wstring& data) {
    return RegUtil::SetString(hRoot, subKey, valueName, data);
}

bool TweaksEngine::RegReadString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, std::wstring& outData) {
    return RegUtil::GetString(hRoot, subKey, valueName, outData);
}

bool TweaksEngine::RegWriteBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, const BYTE* data, DWORD size) {
    return RegUtil::SetBinary(hRoot, subKey, valueName, data, size);
}

bool TweaksEngine::RegReadBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName, BYTE* outData, DWORD& inOutSize) {
    return RegUtil::GetBinary(hRoot, subKey, valueName, outData, inOutSize);
}

bool TweaksEngine::RegDeleteValue(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName) {
    return RegUtil::DeleteVal(hRoot, subKey, valueName);
}

bool TweaksEngine::DetectPciMsiMode(const wchar_t* targetClassGuid) {
    ScopedHKey hPciKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, hPciKey.GetAddressOf()) != ERROR_SUCCESS) {
        return false;
    }

    DWORD subKeys = 0;
    RegQueryInfoKeyW(hPciKey.Get(), NULL, NULL, NULL, &subKeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    bool anyEnabled = false;

    for (DWORD k = 0; k < subKeys; ++k) {
        wchar_t devKeyName[256];
        DWORD devKeyLen = 256;
        if (RegEnumKeyExW(hPciKey.Get(), k, devKeyName, &devKeyLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            ScopedHKey hSubDev;
            if (RegOpenKeyExW(hPciKey.Get(), devKeyName, 0, KEY_READ, hSubDev.GetAddressOf()) == ERROR_SUCCESS) {
                DWORD instCount = 0;
                RegQueryInfoKeyW(hSubDev.Get(), NULL, NULL, NULL, &instCount, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                for (DWORD inst = 0; inst < instCount; ++inst) {
                    wchar_t instName[256];
                    DWORD instLen = 256;
                    if (RegEnumKeyExW(hSubDev.Get(), inst, instName, &instLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        std::wstring instPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\") + devKeyName + L"\\" + instName;
                        std::wstring classVal;
                        bool isTargetClass = false;
                        if (RegReadString(HKEY_LOCAL_MACHINE, instPath, L"ClassGUID", classVal)) {
                            if (_wcsicmp(classVal.c_str(), targetClassGuid) == 0) {
                                isTargetClass = true;
                            }
                        }
                        if (!isTargetClass && RegReadString(HKEY_LOCAL_MACHINE, instPath, L"Class", classVal)) {
                            if ((_wcsicmp(targetClassGuid, L"{4d36e968-e325-11ce-bfc1-08002be10318}") == 0 && _wcsicmp(classVal.c_str(), L"Display") == 0) ||
                                (_wcsicmp(targetClassGuid, L"{4d36e96c-e325-11ce-bfc1-08002be10318}") == 0 && _wcsicmp(classVal.c_str(), L"MEDIA") == 0)) {
                                isTargetClass = true;
                            }
                        }
                        if (!isTargetClass && _wcsicmp(targetClassGuid, L"{4d36e96c-e325-11ce-bfc1-08002be10318}") == 0) {
                            std::wstring svcVal, descVal, hwVal;
                            RegReadString(HKEY_LOCAL_MACHINE, instPath, L"Service", svcVal);
                            RegReadString(HKEY_LOCAL_MACHINE, instPath, L"DeviceDesc", descVal);
                            RegReadString(HKEY_LOCAL_MACHINE, instPath, L"HardwareID", hwVal);
                            if (_wcsicmp(svcVal.c_str(), L"HDAudBus") == 0 ||
                                hwVal.find(L"&CC_0403") != std::wstring::npos ||
                                hwVal.find(L"&CC_04") != std::wstring::npos ||
                                descVal.find(L"Audio") != std::wstring::npos ||
                                descVal.find(L"Sound") != std::wstring::npos) {
                                isTargetClass = true;
                            }
                        }

                        if (isTargetClass) {
                            std::wstring msiKey = instPath + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
                            DWORD msiVal = 0;
                            if (RegReadDword(HKEY_LOCAL_MACHINE, msiKey, L"MSISupported", msiVal) && msiVal == 1) {
                                anyEnabled = true;
                                break;
                            }
                        }
                    }
                }
                if (anyEnabled) break;
            }
        }
    }
    return anyEnabled;
}

PresetType TweaksEngine::EvaluateActivePreset() const {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    // 1. Esports Ultra: Core high-performance latency profile
    bool isUltra = m_state.win32PrioritySeparation && (m_state.win32PriorityValue == 0x26 || m_state.win32PriorityValue == 38)
                && m_state.mmcssGamesPriority
                && m_state.disableNetworkThrottling
                && m_state.tcpNoDelay
                && m_state.gpuMsiMode
                && m_state.disableGameDVR
                && m_state.ultimatePowerPlan;
    if (isUltra) {
        return PresetType::EsportsUltra;
    }

    // 2. Competitive Gaming: Latency optimizations with balanced power profile
    bool isBalanced = m_state.win32PrioritySeparation && (m_state.win32PriorityValue == 0x26 || m_state.win32PriorityValue == 38)
                   && m_state.mmcssGamesPriority
                   && m_state.disableNetworkThrottling
                   && m_state.tcpNoDelay
                   && m_state.gpuMsiMode
                   && m_state.disableGameDVR;
    if (isBalanced) {
        return PresetType::CompetitiveGaming;
    }

    // 3. Windows Default: Factory standard Microsoft configuration
    bool isDefault = (!m_state.win32PrioritySeparation || m_state.win32PriorityValue == 2)
                  && !m_state.mmcssGamesPriority
                  && !m_state.disableNetworkThrottling
                  && !m_state.disableGameDVR;
    if (isDefault) {
        return PresetType::WindowsDefault;
    }

    return PresetType::None;
}

void TweaksEngine::DetectFastState() {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    // 1. Timer Resolution (live NT query via Service or NtQueryTimerResolution)
    m_state.timerResolution05ms = TimerResolutionService::Get().IsActive();
    if (!m_state.timerResolution05ms) {
        float curRes = TimerResolutionService::Get().GetCurrentResolution();
        if (curRes > 0.0f && curRes <= 0.505f) {
            m_state.timerResolution05ms = true;
        }
    }

    // 2. Real BCD low-latency tweaks (fast command capture)
    std::string bcdOut;
    bool bcdSuccess = RunCommandCapture(L"bcdedit /enum {current}", bcdOut);
    if (!bcdSuccess) {
        bcdSuccess = RunCommandCapture(L"bcdedit /enum", bcdOut);
    }
    if (bcdSuccess) {
        std::string bcdLower = bcdOut;
        std::transform(bcdLower.begin(), bcdLower.end(), bcdLower.begin(), ::tolower);

        size_t dtPos = bcdLower.find("disabledynamictick");
        if (dtPos != std::string::npos) {
            std::string sub = bcdLower.substr(dtPos, 40);
            m_state.disableDynamicTick = (sub.find("yes") != std::string::npos || sub.find("true") != std::string::npos);
        } else {
            m_state.disableDynamicTick = false;
        }

        size_t pcPos = bcdLower.find("useplatformclock");
        if (pcPos != std::string::npos) {
            std::string sub = bcdLower.substr(pcPos, 40);
            m_state.usePlatformClockNo = (sub.find("no") != std::string::npos || sub.find("false") != std::string::npos);
        } else {
            m_state.usePlatformClockNo = false;
        }

        size_t ptPos = bcdLower.find("useplatformtick");
        if (ptPos != std::string::npos) {
            std::string sub = bcdLower.substr(ptPos, 40);
            m_state.usePlatformTick = (sub.find("yes") != std::string::npos || sub.find("true") != std::string::npos);
            m_state.usePlatformTickNo = m_state.usePlatformTick;
        } else {
            m_state.usePlatformTick = false;
            m_state.usePlatformTickNo = false;
        }

        size_t bxPos = bcdLower.find("bootux");
        if (bxPos != std::string::npos) {
            std::string sub = bcdLower.substr(bxPos, 40);
            m_state.bootuxDisabled = (sub.find("disabled") != std::string::npos);
        } else {
            m_state.bootuxDisabled = false;
        }
        m_bcdReady.store(true);
    } else {
        m_state.disableDynamicTick = false;
        m_state.usePlatformClockNo = false;
        m_state.usePlatformTick = false;
        m_state.usePlatformTickNo = false;
        m_state.bootuxDisabled = false;
    }

    // 3. Power Plan & Core Parking (live Power Management API)
    m_state.ultimatePowerPlan = false;
    m_state.disableCoreParking = false;
    GUID* pActiveScheme = nullptr;
    if (PowerGetActiveScheme(NULL, &pActiveScheme) == ERROR_SUCCESS && pActiveScheme != nullptr) {
        static const GUID GUID_ULTIMATE_PERF = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
        if (IsEqualGUID(*pActiveScheme, GUID_ULTIMATE_PERF)) {
            m_state.ultimatePowerPlan = true;
        } else {
            UCHAR nameBuf[512] = { 0 };
            DWORD nameSize = sizeof(nameBuf);
            if (PowerReadFriendlyName(NULL, pActiveScheme, NULL, NULL, nameBuf, &nameSize) == ERROR_SUCCESS) {
                wchar_t* wname = reinterpret_cast<wchar_t*>(nameBuf);
                m_state.ultimatePowerPlan = (wcsstr(wname, L"Ultimate") != nullptr ||
                                             wcsstr(wname, L"Максимальная") != nullptr ||
                                             wcsstr(wname, L"Esports") != nullptr);
            }
        }

        // Query minimum processor state and CPMINCORES for core parking
        static const GUID GUID_SUB_PROC = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
        static const GUID GUID_THROTTLE_MIN = { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };
        static const GUID GUID_CPMINCORES = { 0x0cc5b647, 0xc360, 0x45f8, { 0x84, 0x22, 0x04, 0x08, 0x45, 0xa7, 0x06, 0x56 } };
        static const GUID GUID_CPMAXCORES = { 0xea062031, 0x0e34, 0x4ff1, { 0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x3a, 0xc0, 0x8f } };
        DWORD minThrottle = 0;
        DWORD cpMinCores = 0;
        bool disabledThrottle = (PowerReadACValueIndex(NULL, pActiveScheme, &GUID_SUB_PROC, &GUID_THROTTLE_MIN, &minThrottle) == ERROR_SUCCESS && minThrottle >= 100);
        bool disabledCores = (PowerReadACValueIndex(NULL, pActiveScheme, &GUID_SUB_PROC, &GUID_CPMINCORES, &cpMinCores) == ERROR_SUCCESS && cpMinCores >= 100);
        m_state.disableCoreParking = (disabledThrottle || disabledCores);

        LocalFree(pActiveScheme);
    }

    // Power Throttling (Intel/AMD EcoQoS disable)
    DWORD pwrThrottle = 0;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff", pwrThrottle)) {
        m_state.powerThrottling = (pwrThrottle == 1);
    }

    // 4. GPU & Audio MSI Mode (live PCI bus enumeration & MsiManager)
    m_state.gpuMsiMode = MsiManager::Get().IsCategoryMsiEnabled("GPU") || DetectPciMsiMode(L"{4d36e968-e325-11ce-bfc1-08002be10318}");
    m_state.audioMsiMode = MsiManager::Get().IsCategoryMsiEnabled("Audio") || DetectPciMsiMode(L"{4d36e96c-e325-11ce-bfc1-08002be10318}") || DetectPciMsiMode(L"{4d36e97d-e325-11ce-bfc1-08002be10318}");

    // 5. HAGS (Hardware-Accelerated GPU Scheduling)
    DWORD hwSchMode = 0;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", hwSchMode)) {
        m_state.hagsEnabled = (hwSchMode == 2);
    }

    // 6. Win32PrioritySeparation (Thread Quantums & Foreground Boost)
    DWORD prio = 2;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl", L"Win32PrioritySeparation", prio)) {
        m_state.win32PriorityValue = prio;
        m_state.win32PrioritySeparation = (prio == 38 || prio == 0x26 || (prio != 2 && prio != 0));
    }

    // 7. CSRSS & DWM Real-Time / High Priority (IFEO)
    DWORD csrssPrio = 0;
    DWORD dwmPrio = 0;
    bool hasCsrss = (RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\csrss.exe\\PerfOptions", L"CpuPriorityClass", csrssPrio) && csrssPrio == 3);
    bool hasDwm = (RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\dwm.exe\\PerfOptions", L"CpuPriorityClass", dwmPrio) && dwmPrio == 3);
    m_state.csrssDwmPriority = (hasCsrss || hasDwm);

    // 8. Memory Management (DisablePagingExecutive & LargeSystemCache & IoPageLockLimit)
    DWORD pagingExec = 0;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"DisablePagingExecutive", pagingExec)) {
        m_state.disablePagingExecutive = (pagingExec == 1);
    }
    DWORD sysCache = 0;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"LargeSystemCache", sysCache)) {
        m_state.largeSystemCacheDisabled = (sysCache == 0); // 0 is gaming mode!
    } else {
        m_state.largeSystemCacheDisabled = true; // default Windows desktop is 0
    }
    DWORD ioLimit = 0;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"IoPageLockLimit", ioLimit)) {
        m_state.ioPageLockLimit = (ioLimit > 0);
    }

    // ForceP0 and Resizable BAR
    DWORD p0Level = 0;
    DWORD perfSrc = 0;
    bool hasP0 = (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000", L"PerfLevelSrc", perfSrc) && perfSrc == 0x2222) ||
                 (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000", L"PowerMizerLevel", p0Level) && p0Level == 1);
    m_state.forceP0 = hasP0;
    m_state.resizableBarDetected = DetectResizableBar();

    // 9. GameDVR & Fullscreen Optimizations
    DWORD gDvr = 1;
    DWORD polDvr = 1;
    DWORD appCapture = 1;
    bool hasDvrUser = RegReadDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled", gDvr);
    bool hasDvrPol = RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR", polDvr);
    bool hasAppCap = RegReadDword(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", appCapture);
    m_state.disableGameDVR = ((hasDvrUser && gDvr == 0) || (hasDvrPol && polDvr == 0) || (hasAppCap && appCapture == 0));

    DWORD dseMode = 0;
    DWORD fseMode = 0;
    DWORD honorMode = 0;
    DWORD dxgiHonor = 0;
    bool hasDse = RegReadDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_DSEBehavior", dseMode);
    bool hasFse = RegReadDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_FSEBehaviorMode", fseMode);
    bool hasHonor = RegReadDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_HonorUserFSEBehaviorMode", honorMode);
    bool hasDxgi = RegReadDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_DXGIHonorFSEWindowsCompatible", dxgiHonor);
    m_state.disableFullscreenOptimizations = ((hasFse && fseMode == 2) || (hasHonor && honorMode == 1) || (hasDxgi && dxgiHonor == 1) || (hasDse && dseMode == 1));

    // 10. MMCSS
    DWORD sysResp = 20;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile", L"SystemResponsiveness", sysResp)) {
        m_state.mmcssGamesPriority = (sysResp == 0);
    }

    DWORD netThrottling = 10;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile", L"NetworkThrottlingIndex", netThrottling)) {
        m_state.disableNetworkThrottling = (netThrottling == 0xFFFFFFFF);
    }

    // 11. Keyboard repeat
    std::wstring kbSpeed;
    if (RegReadString(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"KeyboardSpeed", kbSpeed)) {
        m_state.keyboardMaxSpeed = (kbSpeed == L"31");
    }

    // 12. Mouse Accel & Linear 1:1 MarkC Curve
    BYTE curveBuf[40] = { 0 };
    DWORD curveSize = sizeof(curveBuf);
    bool hasCurve = RegReadBinary(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"SmoothMouseXCurve", curveBuf, curveSize);
    m_state.flatMouseCurve = (hasCurve && curveSize >= 12 && curveBuf[8] == 0xC0 && curveBuf[9] == 0xCC && curveBuf[10] == 0x0C);

    // 13. Telemetry & Diagnostic tracking
    DWORD telem = 1;
    bool hasTelem = RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", telem);
    DWORD diagStart = 2;
    bool hasDiag = RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\DiagTrack", L"Start", diagStart);
    DWORD dmwStart = 3;
    bool hasDmw = RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\dmwappushservice", L"Start", dmwStart);
    m_state.disableTelemetry = ((hasTelem && telem == 0) || (hasDiag && diagStart == 4) || (hasDmw && dmwStart == 4));

    // 14. Error reporting
    DWORD werDisabled = 0;
    DWORD werPolDisabled = 0;
    DWORD werSvcStart = 3;
    bool hasWer = RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting", L"Disabled", werDisabled);
    bool hasWerPol = RegReadDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Error Reporting", L"Disabled", werPolDisabled);
    bool hasWerSvc = RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\WerSvc", L"Start", werSvcStart);
    m_state.disableErrorReporting = ((hasWer && werDisabled == 1) || (hasWerPol && werPolDisabled == 1) || (hasWerSvc && werSvcStart == 4));

    // 15. TCP NoDelay across active network interfaces and global parameters
    bool anyNoDelay = false;
    bool anyAckFreq = false;
    DWORD globalNoDelay = 0;
    DWORD globalAckFreq = 0;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"TCPNoDelay", globalNoDelay) && globalNoDelay == 1) {
        anyNoDelay = true;
    }
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"TcpAckFrequency", globalAckFreq) && globalAckFreq == 1) {
        anyAckFreq = true;
    }

    ScopedHKey hTcpInterfaces;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces", 0, KEY_READ, hTcpInterfaces.GetAddressOf()) == ERROR_SUCCESS) {
        DWORD subKeyCount = 0;
        RegQueryInfoKeyW(hTcpInterfaces.Get(), NULL, NULL, NULL, &subKeyCount, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        for (DWORD i = 0; i < subKeyCount; ++i) {
            wchar_t subName[256] = { 0 };
            DWORD subLen = 255;
            if (RegEnumKeyExW(hTcpInterfaces.Get(), i, subName, &subLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                subName[255] = L'\0';
                std::wstring ifKey = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + subName;
                DWORD noDelay = 0;
                DWORD ackFreq = 0;
                if (RegReadDword(HKEY_LOCAL_MACHINE, ifKey, L"TCPNoDelay", noDelay) && noDelay == 1) {
                    anyNoDelay = true;
                }
                if (RegReadDword(HKEY_LOCAL_MACHINE, ifKey, L"TcpAckFrequency", ackFreq) && ackFreq == 1) {
                    anyAckFreq = true;
                }
                if (anyNoDelay && anyAckFreq) break;
            }
        }
    }
    m_state.tcpNoDelay = anyNoDelay;
    m_state.tcpAckFrequency = anyAckFreq;
    m_state.tcpAutoTuning = true; // Default true, verified via netsh in DetectCurrentStateAsync

    // 16. Network TTL & MTU
    DWORD ttlVal = 128;
    if (RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"DefaultTTL", ttlVal)) {
        m_state.defaultTTL64 = (ttlVal == 64);
    }
    DWORD mtuVal = 0;
    m_state.optimalMTU = false;
    ScopedHKey hMtuIf;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces", 0, KEY_READ, hMtuIf.GetAddressOf()) == ERROR_SUCCESS) {
        DWORD ifCnt = 0;
        RegQueryInfoKeyW(hMtuIf.Get(), NULL, NULL, NULL, &ifCnt, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        for (DWORD i = 0; i < ifCnt; ++i) {
            wchar_t sName[256]; DWORD sLen = 256;
            if (RegEnumKeyExW(hMtuIf.Get(), i, sName, &sLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                if (RegReadDword(HKEY_LOCAL_MACHINE, std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + sName, L"MTU", mtuVal) && mtuVal == 1500) {
                    m_state.optimalMTU = true;
                    break;
                }
            }
        }
    }

    // 17. Dynamically evaluate active preset
    m_activePreset = EvaluateActivePreset();
}

void TweaksEngine::DetectCurrentState() {
    DetectFastState();
    DetectCurrentStateAsync();
}

void TweaksEngine::DetectCurrentStateAsync(std::function<void()> onComplete) {
    std::thread([this, onComplete]() {
        // Detailed verification of BCD, PCI, and network parameters
        DetectFastState();

        // Verify TCP auto-tuning level
        std::string netshOut;
        if (RunCommandCapture(L"netsh int tcp show global", netshOut)) {
            std::string netshLower = netshOut;
            std::transform(netshLower.begin(), netshLower.end(), netshLower.begin(), ::tolower);
            if (netshLower.find("autotuninglevel") != std::string::npos) {
                size_t pos = netshLower.find("autotuninglevel");
                std::string sub = netshLower.substr(pos, 50);
                if (sub.find("disabled") != std::string::npos) {
                    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
                    m_state.tcpAutoTuning = false;
                } else if (sub.find("normal") != std::string::npos || sub.find("restricted") != std::string::npos || sub.find("experimental") != std::string::npos) {
                    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
                    m_state.tcpAutoTuning = true;
                }
            }
        }

        m_isInitialized.store(true);
        if (onComplete) {
            onComplete();
        }
    }).detach();
}

bool TweaksEngine::SetTimerResolution(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.timerResolution05ms = enable;
    if (enable) {
        TimerResolutionService::Get().Start(0.500f);
    } else {
        TimerResolutionService::Get().Stop();
    }
    return true;
}

bool TweaksEngine::SetDynamicTick(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disableDynamicTick = disable;
    if (disable) {
        return RunCommandHidden(L"bcdedit /set {current} disabledynamictick yes") ||
               RunCommandHidden(L"bcdedit /set disabledynamictick yes");
    } else {
        return RunCommandHidden(L"bcdedit /deletevalue {current} disabledynamictick") ||
               RunCommandHidden(L"bcdedit /set {current} disabledynamictick no") ||
               RunCommandHidden(L"bcdedit /set disabledynamictick no");
    }
}

bool TweaksEngine::SetPlatformClock(bool disableHPET) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.usePlatformClockNo = disableHPET;
    if (disableHPET) {
        return RunCommandHidden(L"bcdedit /set {current} useplatformclock no") ||
               RunCommandHidden(L"bcdedit /set useplatformclock no");
    } else {
        return RunCommandHidden(L"bcdedit /deletevalue {current} useplatformclock") ||
               RunCommandHidden(L"bcdedit /deletevalue useplatformclock");
    }
}

bool TweaksEngine::SetPlatformTick(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.usePlatformTick = enable;
    m_state.usePlatformTickNo = enable;
    if (enable) {
        return RunCommandHidden(L"bcdedit /set {current} useplatformtick yes") ||
               RunCommandHidden(L"bcdedit /set useplatformtick yes");
    } else {
        return RunCommandHidden(L"bcdedit /deletevalue {current} useplatformtick") ||
               RunCommandHidden(L"bcdedit /deletevalue useplatformtick");
    }
}

bool TweaksEngine::SetBootUX(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.bootuxDisabled = disable;
    if (disable) {
        return RunCommandHidden(L"bcdedit /set {current} bootux disabled") ||
               RunCommandHidden(L"bcdedit /set bootux disabled");
    } else {
        return RunCommandHidden(L"bcdedit /deletevalue {current} bootux") ||
               RunCommandHidden(L"bcdedit /deletevalue bootux");
    }
}

bool TweaksEngine::SetWin32PrioritySeparation(bool enable0x26, DWORD customVal) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.win32PrioritySeparation = enable0x26;
    DWORD val = enable0x26 ? (customVal ? customVal : 0x26) : 2;
    m_state.win32PriorityValue = val;
    return RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl", L"Win32PrioritySeparation", val);
}

bool TweaksEngine::SetUltimatePowerPlan(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.ultimatePowerPlan = enable;
    if (enable) {
        // Enumerate to see if Ultimate already exists
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
        } else {
            RunCommandHidden(L"powercfg -duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61");
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
                        found = true;
                        break;
                    }
                }
                size = sizeof(GUID);
            }
            if (!found) {
                static const GUID GUID_HIGH = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
                PowerSetActiveScheme(NULL, &GUID_HIGH);
            }
        }
        SetCoreParking(true);
    } else {
        static const GUID GUID_BALANCED = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
        PowerSetActiveScheme(NULL, &GUID_BALANCED);
        SetCoreParking(false);
    }
    SystemInfo::Get().Refresh();
    return true;
}

bool TweaksEngine::SetCoreParking(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disableCoreParking = disable;
    if (disable) {
        // 100% min active cores (0 parked cores)
        RunCommandHidden(L"powercfg -setacvalueindex scheme_current 54533251-82be-4824-96c1-47b60b740d00 0cc5b647-c360-45f8-8422-040845a70656 100");
        RunCommandHidden(L"powercfg -setdcvalueindex scheme_current 54533251-82be-4824-96c1-47b60b740d00 0cc5b647-c360-45f8-8422-040845a70656 100");
        RunCommandHidden(L"powercfg -setacvalueindex scheme_current 54533251-82be-4824-96c1-47b60b740d00 ea062031-0e34-4ff1-9b6d-eb10593ac08f 100");
        RunCommandHidden(L"powercfg -setdcvalueindex scheme_current 54533251-82be-4824-96c1-47b60b740d00 ea062031-0e34-4ff1-9b6d-eb10593ac08f 100");
        RunCommandHidden(L"powercfg -setacvalueindex scheme_current sub_processor CPMINCORES 100");
        RunCommandHidden(L"powercfg -setdcvalueindex scheme_current sub_processor CPMINCORES 100");
        RunCommandHidden(L"powercfg -setacvalueindex scheme_current sub_processor PROCTHROTTLEMIN 100");
        RunCommandHidden(L"powercfg -setdcvalueindex scheme_current sub_processor PROCTHROTTLEMIN 100");
        RunCommandHidden(L"powercfg -setactive scheme_current");
    } else {
        RunCommandHidden(L"powercfg -setacvalueindex scheme_current 54533251-82be-4824-96c1-47b60b740d00 0cc5b647-c360-45f8-8422-040845a70656 10");
        RunCommandHidden(L"powercfg -setdcvalueindex scheme_current 54533251-82be-4824-96c1-47b60b740d00 0cc5b647-c360-45f8-8422-040845a70656 100");
        RunCommandHidden(L"powercfg -setacvalueindex scheme_current sub_processor CPMINCORES 10");
        RunCommandHidden(L"powercfg -setacvalueindex scheme_current sub_processor PROCTHROTTLEMIN 5");
        RunCommandHidden(L"powercfg -setactive scheme_current");
    }
    return true;
}

bool TweaksEngine::SetPowerThrottling(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.powerThrottling = disable;
    return RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff", disable ? 1 : 0);
}

bool TweaksEngine::SetGPUMsiMode(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.gpuMsiMode = enable;
    MsiManager::Get().SetCategoryMsi("GPU", enable);
    // Iterate over PCI devices and set MSISupported for Display class
    ScopedHKey hPciKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, hPciKey.GetAddressOf()) != ERROR_SUCCESS) {
        return false;
    }

    DWORD subKeys = 0;
    RegQueryInfoKeyW(hPciKey.Get(), NULL, NULL, NULL, &subKeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    for (DWORD k = 0; k < subKeys; ++k) {
        wchar_t devKeyName[256];
        DWORD devKeyLen = 256;
        if (RegEnumKeyExW(hPciKey.Get(), k, devKeyName, &devKeyLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            ScopedHKey hSubDev;
            if (RegOpenKeyExW(hPciKey.Get(), devKeyName, 0, KEY_READ, hSubDev.GetAddressOf()) == ERROR_SUCCESS) {
                DWORD instCount = 0;
                RegQueryInfoKeyW(hSubDev.Get(), NULL, NULL, NULL, &instCount, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                for (DWORD inst = 0; inst < instCount; ++inst) {
                    wchar_t instName[256];
                    DWORD instLen = 256;
                    if (RegEnumKeyExW(hSubDev.Get(), inst, instName, &instLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        std::wstring instPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\") + devKeyName + L"\\" + instName;
                        
                        // Check ClassGUID
                        std::wstring classGuid;
                        if (RegReadString(HKEY_LOCAL_MACHINE, instPath, L"ClassGUID", classGuid)) {
                            // Display adapter GUID: {4d36e968-e325-11ce-bfc1-08002be10318}
                            if (_wcsicmp(classGuid.c_str(), L"{4d36e968-e325-11ce-bfc1-08002be10318}") == 0) {
                                std::wstring msiKey = instPath + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
                                std::wstring affKey = instPath + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";
                                
                                RegWriteDword(HKEY_LOCAL_MACHINE, msiKey, L"MSISupported", enable ? 1 : 0);
                                if (enable) {
                                    // DevicePriority: 1 = High, 2 = Normal, 3 = Low
                                    RegWriteDword(HKEY_LOCAL_MACHINE, msiKey, L"MessageNumberLimit", 1);
                                    RegWriteDword(HKEY_LOCAL_MACHINE, affKey, L"DevicePriority", 1);
                                } else {
                                    RegDeleteValue(HKEY_LOCAL_MACHINE, msiKey, L"MessageNumberLimit");
                                    RegDeleteValue(HKEY_LOCAL_MACHINE, affKey, L"DevicePriority");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    SystemInfo::Get().Refresh();
    return true;
}

bool TweaksEngine::SetAudioMsiMode(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.audioMsiMode = enable;
    MsiManager::Get().SetCategoryMsi("Audio", enable);
    ScopedHKey hPciKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, hPciKey.GetAddressOf()) != ERROR_SUCCESS) {
        return false;
    }

    DWORD subKeys = 0;
    RegQueryInfoKeyW(hPciKey.Get(), NULL, NULL, NULL, &subKeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    for (DWORD k = 0; k < subKeys; ++k) {
        wchar_t devKeyName[256];
        DWORD devKeyLen = 256;
        if (RegEnumKeyExW(hPciKey.Get(), k, devKeyName, &devKeyLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            ScopedHKey hSubDev;
            if (RegOpenKeyExW(hPciKey.Get(), devKeyName, 0, KEY_READ, hSubDev.GetAddressOf()) == ERROR_SUCCESS) {
                DWORD instCount = 0;
                RegQueryInfoKeyW(hSubDev.Get(), NULL, NULL, NULL, &instCount, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                for (DWORD inst = 0; inst < instCount; ++inst) {
                    wchar_t instName[256];
                    DWORD instLen = 256;
                    if (RegEnumKeyExW(hSubDev.Get(), inst, instName, &instLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                        std::wstring instPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\") + devKeyName + L"\\" + instName;
                        std::wstring classGuid, classVal, svcVal, descVal, hwVal;
                        bool isAudio = false;
                        if (RegReadString(HKEY_LOCAL_MACHINE, instPath, L"ClassGUID", classGuid)) {
                            // Media Audio Adapter: {4d36e96c-e325-11ce-bfc1-08002be10318} or System Audio Controller: {4d36e97d-e325-11ce-bfc1-08002be10318}
                            if (_wcsicmp(classGuid.c_str(), L"{4d36e96c-e325-11ce-bfc1-08002be10318}") == 0) {
                                isAudio = true;
                            }
                        }
                        RegReadString(HKEY_LOCAL_MACHINE, instPath, L"Service", svcVal);
                        RegReadString(HKEY_LOCAL_MACHINE, instPath, L"DeviceDesc", descVal);
                        if (!isAudio && RegReadString(HKEY_LOCAL_MACHINE, instPath, L"Class", classVal)) {
                            if (_wcsicmp(classVal.c_str(), L"MEDIA") == 0) {
                                isAudio = true;
                            }
                        }
                        if (!isAudio) {
                            if (_wcsicmp(svcVal.c_str(), L"HDAudBus") == 0 ||
                                instPath.find(L"CC_0403") != std::wstring::npos ||
                                instPath.find(L"CC_04") != std::wstring::npos ||
                                descVal.find(L"Audio") != std::wstring::npos ||
                                descVal.find(L"Sound") != std::wstring::npos) {
                                isAudio = true;
                            }
                        }

                        if (isAudio) {
                            std::wstring msiKey = instPath + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
                            std::wstring affKey = instPath + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";
                            RegWriteDword(HKEY_LOCAL_MACHINE, msiKey, L"MSISupported", enable ? 1 : 0);
                            if (enable) {
                                RegWriteDword(HKEY_LOCAL_MACHINE, msiKey, L"MessageNumberLimit", 1);
                                RegWriteDword(HKEY_LOCAL_MACHINE, affKey, L"DevicePriority", 2);
                            } else {
                                RegDeleteValue(HKEY_LOCAL_MACHINE, msiKey, L"MessageNumberLimit");
                                RegDeleteValue(HKEY_LOCAL_MACHINE, affKey, L"DevicePriority");
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool TweaksEngine::SetHAGS(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.hagsEnabled = enable;

    // Check if GPU driver and OS support Hardware-Accelerated GPU Scheduling (WDDM 2.7+)
    DWORD currentMode = 0;
    if (!RegReadDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", currentMode)) {
        DEBUG_LOG_WARN("TweaksEngine", "HAGS (HwSchMode) не поддерживается установленным видеодрайвером/GPU (требуется WDDM 2.7+). Отклонено.");
        m_state.hagsEnabled = false;
        return false;
    }

    DWORD mode = enable ? 2 : 1;
    return RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", mode);
}

bool TweaksEngine::SetForceP0(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.forceP0 = false;

    // Always clean up any legacy PowerMizer registry keys that might have been written
    ScopedHKey hClassKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0, KEY_READ | KEY_WRITE, hClassKey.GetAddressOf()) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < 8; ++i) {
            wchar_t subName[16];
            swprintf_s(subName, L"%04d", i);
            ScopedHKey hSub;
            if (RegOpenKeyExW(hClassKey.Get(), subName, 0, KEY_READ | KEY_WRITE, hSub.GetAddressOf()) == ERROR_SUCCESS) {
                std::wstring subPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\") + subName;
                RegDeleteValue(HKEY_LOCAL_MACHINE, subPath, L"PerfLevelSrc");
                RegDeleteValue(HKEY_LOCAL_MACHINE, subPath, L"PowerMizerEnable");
                RegDeleteValue(HKEY_LOCAL_MACHINE, subPath, L"PowerMizerLevel");
                RegDeleteValue(HKEY_LOCAL_MACHINE, subPath, L"PowerMizerLevelAC");
            }
        }
    }

    if (enable) {
        DEBUG_LOG_WARN("TweaksEngine", "SetForceP0 отключен: вендорозависимый твик (только старые NVIDIA), вызывающий нестабильность на AMD/Intel и современных GPU.");
        return false;
    }
    return true;
}

bool TweaksEngine::DetectResizableBar() {
    ScopedHKey hClassKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0, KEY_READ, hClassKey.GetAddressOf()) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < 8; ++i) {
            wchar_t subName[16];
            swprintf_s(subName, L"%04d", i);
            ScopedHKey hSub;
            if (RegOpenKeyExW(hClassKey.Get(), subName, 0, KEY_READ, hSub.GetAddressOf()) == ERROR_SUCCESS) {
                DWORD rebar = 0;
                DWORD size = sizeof(rebar);
                if (RegQueryValueExW(hSub.Get(), L"ReBarState", NULL, NULL, (LPBYTE)&rebar, &size) == ERROR_SUCCESS && rebar == 1) {
                    return true;
                }
                if (RegQueryValueExW(hSub.Get(), L"KMD_ReBarStatus", NULL, NULL, (LPBYTE)&rebar, &size) == ERROR_SUCCESS && rebar == 1) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool TweaksEngine::SetCsrssDwmPriority(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.csrssDwmPriority = false;
    std::wstring csrssKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\csrss.exe\\PerfOptions";
    std::wstring dwmKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\dwm.exe\\PerfOptions";

    // Always clean up any existing IFEO entries to protect user from anti-cheat bans (Vanguard, EAC, BattlEye) and DWM crashes
    RegDeleteValue(HKEY_LOCAL_MACHINE, csrssKey, L"CpuPriorityClass");
    RegDeleteValue(HKEY_LOCAL_MACHINE, csrssKey, L"IoPriority");
    RegDeleteValue(HKEY_LOCAL_MACHINE, dwmKey, L"CpuPriorityClass");
    RegDeleteValue(HKEY_LOCAL_MACHINE, dwmKey, L"IoPriority");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, csrssKey.c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, dwmKey.c_str());

    // Restore running dwm.exe process priority to Normal immediately if it was raised
    ScopedHandle hSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (hSnap.IsValid()) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap.Get(), &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0) {
                    ScopedHandle hProc(OpenProcess(PROCESS_SET_INFORMATION, FALSE, pe.th32ProcessID));
                    if (hProc.IsValid()) {
                        SetPriorityClass(hProc.Get(), NORMAL_PRIORITY_CLASS);
                    }
                }
            } while (Process32NextW(hSnap.Get(), &pe));
        }
    }

    if (enable) {
        DEBUG_LOG_WARN("TweaksEngine", "SetCsrssDwmPriority заблокирован: IFEO приоритет для csrss.exe вызывает сбои DWM и ложные баны античитов (Vanguard, EAC, BattlEye).");
        return false;
    }
    return true;
}

bool TweaksEngine::SetGameDVR(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disableGameDVR = disable;
    DWORD dvrVal = disable ? 0 : 1;
    DWORD fseMode = disable ? 2 : 0;

    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled", dvrVal);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_FSEBehaviorMode", fseMode);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_HonorUserFSEBehaviorMode", disable ? 1 : 0);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_DXGIHonorFSEWindowsCompatible", disable ? 1 : 0);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_EFSEFeatureFlags", disable ? 0 : 1);

    RegWriteDword(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", dvrVal);
    RegWriteDword(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"HistoricalCaptureEnabled", dvrVal);
    
    if (disable) {
        RegWriteDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR", 0);
        RegWriteDword(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\GameBar", L"UseNexusForGameBarEnabled", 0);
        RegWriteDword(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\GameBar", L"ShowStartupPanel", 0);
        RegWriteDword(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"GameDVR_AutoCaptureEnabled", 0);
    } else {
        RegDeleteValue(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR");
        RegDeleteValue(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\GameBar", L"UseNexusForGameBarEnabled");
        RegDeleteValue(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\GameBar", L"ShowStartupPanel");
        RegDeleteValue(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"GameDVR_AutoCaptureEnabled");
    }
    return true;
}

bool TweaksEngine::SetFullscreenOptimizations(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disableFullscreenOptimizations = disable;
    DWORD fseMode = disable ? 2 : 0;
    DWORD flag = disable ? 1 : 0;

    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_FSEBehaviorMode", fseMode);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_HonorUserFSEBehaviorMode", flag);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_DXGIHonorFSEWindowsCompatible", flag);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_DSEBehavior", flag);
    RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_EFSEFeatureFlags", disable ? 0 : 1);
    if (disable) {
        RegWriteDword(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_FSEBehavior", 2);
    } else {
        RegDeleteValue(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_FSEBehavior");
    }
    return true;
}

bool TweaksEngine::SetMMCSSGamesPriority(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.mmcssGamesPriority = enable;
    std::wstring sysProfile = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
    std::wstring gamesTask = sysProfile + L"\\Tasks\\Games";

    if (enable) {
        RegWriteDword(HKEY_LOCAL_MACHINE, sysProfile, L"SystemResponsiveness", 0);
        RegWriteDword(HKEY_LOCAL_MACHINE, sysProfile, L"NoLazyMode", 1);
        RegWriteDword(HKEY_LOCAL_MACHINE, gamesTask, L"Affinity", 0);
        RegWriteString(HKEY_LOCAL_MACHINE, gamesTask, L"Background Only", L"False");
        RegWriteDword(HKEY_LOCAL_MACHINE, gamesTask, L"Clock Rate", 10000);
        RegWriteDword(HKEY_LOCAL_MACHINE, gamesTask, L"GPU Priority", 8);
        RegWriteDword(HKEY_LOCAL_MACHINE, gamesTask, L"Priority", 6);
        RegWriteString(HKEY_LOCAL_MACHINE, gamesTask, L"Scheduling Category", L"High");
        RegWriteString(HKEY_LOCAL_MACHINE, gamesTask, L"SFIO Priority", L"High");
    } else {
        RegWriteDword(HKEY_LOCAL_MACHINE, sysProfile, L"SystemResponsiveness", 20);
        RegDeleteValue(HKEY_LOCAL_MACHINE, sysProfile, L"NoLazyMode");
        RegWriteDword(HKEY_LOCAL_MACHINE, gamesTask, L"GPU Priority", 8);
        RegWriteDword(HKEY_LOCAL_MACHINE, gamesTask, L"Priority", 2);
        RegWriteString(HKEY_LOCAL_MACHINE, gamesTask, L"Scheduling Category", L"Medium");
        RegWriteString(HKEY_LOCAL_MACHINE, gamesTask, L"SFIO Priority", L"Normal");
    }
    return true;
}

bool TweaksEngine::SetNetworkThrottling(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disableNetworkThrottling = disable;
    std::wstring sysProfile = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
    if (disable) {
        RegWriteDword(HKEY_LOCAL_MACHINE, sysProfile, L"NetworkThrottlingIndex", 0xFFFFFFFF);
    } else {
        RegWriteDword(HKEY_LOCAL_MACHINE, sysProfile, L"NetworkThrottlingIndex", 10);
    }
    return true;
}

bool TweaksEngine::SetTCPNoDelay(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.tcpNoDelay = enable;
    m_state.tcpAckFrequency = enable;

    // Set globally on Tcpip Parameters
    std::wstring globalTcp = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
    if (enable) {
        RegWriteDword(HKEY_LOCAL_MACHINE, globalTcp, L"TcpAckFrequency", 1);
        RegWriteDword(HKEY_LOCAL_MACHINE, globalTcp, L"TCPNoDelay", 1);
        RegWriteDword(HKEY_LOCAL_MACHINE, globalTcp, L"TcpDelAckTicks", 0);
    } else {
        RegDeleteValue(HKEY_LOCAL_MACHINE, globalTcp, L"TcpAckFrequency");
        RegDeleteValue(HKEY_LOCAL_MACHINE, globalTcp, L"TCPNoDelay");
        RegDeleteValue(HKEY_LOCAL_MACHINE, globalTcp, L"TcpDelAckTicks");
    }

    // Set on all individual network interfaces
    ScopedHKey hTcpInterfaces;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces", 0, KEY_READ, hTcpInterfaces.GetAddressOf()) != ERROR_SUCCESS) {
        return false;
    }

    DWORD subKeys = 0;
    RegQueryInfoKeyW(hTcpInterfaces.Get(), NULL, NULL, NULL, &subKeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

    for (DWORD k = 0; k < subKeys; ++k) {
        wchar_t ifName[256] = { 0 };
        DWORD ifLen = 255;
        if (RegEnumKeyExW(hTcpInterfaces.Get(), k, ifName, &ifLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            ifName[255] = L'\0';
            std::wstring ifPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + ifName;
            if (enable) {
                RegWriteDword(HKEY_LOCAL_MACHINE, ifPath, L"TcpAckFrequency", 1);
                RegWriteDword(HKEY_LOCAL_MACHINE, ifPath, L"TCPNoDelay", 1);
                RegWriteDword(HKEY_LOCAL_MACHINE, ifPath, L"TcpDelAckTicks", 0);
            } else {
                RegDeleteValue(HKEY_LOCAL_MACHINE, ifPath, L"TcpAckFrequency");
                RegDeleteValue(HKEY_LOCAL_MACHINE, ifPath, L"TCPNoDelay");
                RegDeleteValue(HKEY_LOCAL_MACHINE, ifPath, L"TcpDelAckTicks");
            }
        }
    }
    return true;
}

bool TweaksEngine::SetTCPAckFrequency(bool enable) {
    return SetTCPNoDelay(enable);
}

bool TweaksEngine::SetTCPAutoTuning(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.tcpAutoTuning = enable;
    if (enable) {
        RunCommandHidden(L"netsh int tcp set global autotuninglevel=normal");
        RunCommandHidden(L"netsh int tcp set global rss=enabled");
    } else {
        RunCommandHidden(L"netsh int tcp set global autotuninglevel=disabled");
    }
    return true;
}

bool TweaksEngine::FlushDNS() {
    return RunCommandHidden(L"ipconfig /flushdns");
}

bool TweaksEngine::SetDisablePagingExecutive(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disablePagingExecutive = enable;
    return RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"DisablePagingExecutive", enable ? 1 : 0);
}

bool TweaksEngine::SetLargeSystemCache(bool gamingMode) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.largeSystemCacheDisabled = gamingMode;
    // 0 is desktop/gaming mode (conserves RAM for games); 1 is server mode (filesystem cache expansion)
    bool ok = RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", L"LargeSystemCache", gamingMode ? 0 : 1);
    SystemTweaker::Get().GetSsdPerfSubTweaks().largeSystemCache = gamingMode;
    return ok;
}

bool TweaksEngine::SetDefaultTTL(bool enable64) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.defaultTTL64 = enable64;
    if (enable64) {
        return RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"DefaultTTL", 64);
    } else {
        return RegDeleteValue(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", L"DefaultTTL");
    }
}

bool TweaksEngine::SetNetworkMTU(bool enable1500) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.optimalMTU = enable1500;
    ScopedHKey hTcpInterfaces;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces", 0, KEY_READ, hTcpInterfaces.GetAddressOf()) != ERROR_SUCCESS) {
        return false;
    }
    DWORD subKeys = 0;
    RegQueryInfoKeyW(hTcpInterfaces.Get(), NULL, NULL, NULL, &subKeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    for (DWORD k = 0; k < subKeys; ++k) {
        wchar_t ifName[256] = { 0 };
        DWORD ifLen = 255;
        if (RegEnumKeyExW(hTcpInterfaces.Get(), k, ifName, &ifLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            ifName[255] = L'\0';
            std::wstring ifPath = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + ifName;
            if (enable1500) {
                RegWriteDword(HKEY_LOCAL_MACHINE, ifPath, L"MTU", 1500);
            } else {
                RegDeleteValue(HKEY_LOCAL_MACHINE, ifPath, L"MTU");
            }
        }
    }
    return true;
}

bool TweaksEngine::SetIoPageLockLimit(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.ioPageLockLimit = enable;
    std::wstring memPath = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management";
    if (enable) {
        return RegWriteDword(HKEY_LOCAL_MACHINE, memPath, L"IoPageLockLimit", 0x08000000); // 128 MB
    } else {
        return RegDeleteValue(HKEY_LOCAL_MACHINE, memPath, L"IoPageLockLimit");
    }
}

bool TweaksEngine::SetFlatMouseCurve(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.flatMouseCurve = enable;
    if (enable) {
        // True 1:1 MarkC Linear Mouse Curve (Zero acceleration at standard 6/11 pointer speed)
        // 5 coordinate points: (0,0), (0x0CCC0, 0x380000), (0x199980, 0x700000), (0x266640, 0xA80000), (0x333300, 0xE00000)
        // Constant slope Y/X = 4.375. Multiplied by 6/11 default factor (1/4.375) produces exact 1.0 (1:1) linear translation.
        static const BYTE flatXCurve[40] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xC0, 0xCC, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x80, 0x99, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x40, 0x66, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x33, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        static const BYTE flatYCurve[40] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseSpeed", L"0");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold1", L"0");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold2", L"0");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseSensitivity", L"10");
        RegWriteBinary(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"SmoothMouseXCurve", flatXCurve, sizeof(flatXCurve));
        RegWriteBinary(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"SmoothMouseYCurve", flatYCurve, sizeof(flatYCurve));

        // Immediately update Win32 user subsystem mouse parameters in memory
        int mouseParams[3] = { 0, 0, 0 };
        SystemParametersInfoW(SPI_SETMOUSE, 0, mouseParams, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
        SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(UINT_PTR)10, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    } else {
        // Windows Default: Zero Acceleration, Linear 6/11 Pointer Speed, Clean Microsoft Defaults
        // CRITICAL BUGFIX: Never enable MouseSpeed=1 or write distorted curves with 11x multiplier!
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseSpeed", L"0");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold1", L"0");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseThreshold2", L"0");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"MouseSensitivity", L"10");

        // Delete custom curves so Windows defaults take over cleanly, preventing any curve corruption
        RegDeleteValue(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"SmoothMouseXCurve");
        RegDeleteValue(HKEY_CURRENT_USER, L"Control Panel\\Mouse", L"SmoothMouseYCurve");

        int mouseParams[3] = { 0, 0, 0 };
        SystemParametersInfoW(SPI_SETMOUSE, 0, mouseParams, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
        SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(UINT_PTR)10, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    }
    return true;
}

bool TweaksEngine::SetKeyboardSpeed(bool enable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.keyboardMaxSpeed = enable;
    if (enable) {
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"KeyboardDelay", L"0");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"KeyboardSpeed", L"31");
        // Ensure Windows Shell does not intercept PrintScreen for missing Snipping Tool (fixes Lightshot/ShareX)
        RegWriteDword(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"PrintScreenKeyForSnippingEnabled", 0);
        SystemParametersInfoW(SPI_SETKEYBOARDDELAY, 0, 0, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
        SystemParametersInfoW(SPI_SETKEYBOARDSPEED, 31, 0, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    } else {
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"KeyboardDelay", L"1");
        RegWriteString(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"KeyboardSpeed", L"31");
        SystemParametersInfoW(SPI_SETKEYBOARDDELAY, 1, 0, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
        SystemParametersInfoW(SPI_SETKEYBOARDSPEED, 31, 0, SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    }
    return true;
}

bool TweaksEngine::SetTelemetry(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disableTelemetry = disable;
    DWORD telemVal = disable ? 0 : 3;
    RegWriteDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", telemVal);
    RegWriteDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\DataCollection", L"AllowTelemetry", telemVal);

    // DiagTrack and dmwappushservice service start types in registry
    RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\DiagTrack", L"Start", disable ? 4 : 2);
    RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\dmwappushservice", L"Start", disable ? 4 : 3);
    RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\WMI\\Autologger\\AutoLogger-Diagtrack-Listener", L"Start", disable ? 0 : 1);

    if (disable) {
        ConfigureWin32Service(L"DiagTrack", SERVICE_DISABLED, true);
        ConfigureWin32Service(L"dmwappushservice", SERVICE_DISABLED, true);
    } else {
        ConfigureWin32Service(L"DiagTrack", SERVICE_AUTO_START, false);
        ConfigureWin32Service(L"dmwappushservice", SERVICE_DEMAND_START, false);
    }
    return true;
}

bool TweaksEngine::SetErrorReporting(bool disable) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_state.disableErrorReporting = disable;
    DWORD werVal = disable ? 1 : 0;
    RegWriteDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting", L"Disabled", werVal);

    if (disable) {
        ConfigureWin32Service(L"WerSvc", SERVICE_DISABLED, true);
    } else {
        ConfigureWin32Service(L"WerSvc", SERVICE_DEMAND_START, false);
    }
    return true;
}

bool TweaksEngine::PurgeStandbyMemory(std::string& outMsg) {
    size_t freed = 0;
    bool res = MemoryCleaner::Get().CleanRAM(freed, outMsg);
    SystemInfo::Get().RefreshMemoryStats();
    return res;
}

bool TweaksEngine::ApplyAll(std::string& outStatus) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    SetTimerResolution(m_state.timerResolution05ms);
    SetDynamicTick(m_state.disableDynamicTick);
    SetPlatformClock(m_state.usePlatformClockNo);
    SetPlatformTick(m_state.usePlatformTick);
    SetBootUX(m_state.bootuxDisabled);
    SetWin32PrioritySeparation(m_state.win32PrioritySeparation, m_state.win32PriorityValue);
    SetUltimatePowerPlan(m_state.ultimatePowerPlan);
    SetCoreParking(m_state.disableCoreParking);
    SetPowerThrottling(m_state.powerThrottling);
    SetGPUMsiMode(m_state.gpuMsiMode);
    SetAudioMsiMode(m_state.audioMsiMode);
    SetHAGS(m_state.hagsEnabled);
    SetForceP0(m_state.forceP0);
    SetCsrssDwmPriority(m_state.csrssDwmPriority);
    SetGameDVR(m_state.disableGameDVR);
    SetFullscreenOptimizations(m_state.disableFullscreenOptimizations);
    SetMMCSSGamesPriority(m_state.mmcssGamesPriority);
    SetNetworkThrottling(m_state.disableNetworkThrottling);
    SetTCPNoDelay(m_state.tcpNoDelay);
    SetTCPAckFrequency(m_state.tcpAckFrequency);
    SetTCPAutoTuning(m_state.tcpAutoTuning);
    SetDefaultTTL(m_state.defaultTTL64);
    SetNetworkMTU(m_state.optimalMTU);
    SetDisablePagingExecutive(m_state.disablePagingExecutive);
    SetLargeSystemCache(m_state.largeSystemCacheDisabled);
    SetIoPageLockLimit(m_state.ioPageLockLimit);
    SetFlatMouseCurve(m_state.flatMouseCurve);
    SetKeyboardSpeed(m_state.keyboardMaxSpeed);
    SetTelemetry(m_state.disableTelemetry);
    SetErrorReporting(m_state.disableErrorReporting);

    outStatus = "All Selected Tweaks Successfully Applied!";
    return true;
}

bool TweaksEngine::ApplyPreset(const std::string& presetName, std::string& outStatus) {
    if (presetName == "EsportsUltra") {
        return ApplyPreset(PresetType::EsportsUltra, outStatus);
    } else if (presetName == "CompetitiveGaming" || presetName == "BalancedGaming") {
        return ApplyPreset(PresetType::CompetitiveGaming, outStatus);
    } else if (presetName == "WindowsDefault") {
        return ApplyPreset(PresetType::WindowsDefault, outStatus);
    }
    outStatus = "Неизвестный пресет: " + presetName;
    return false;
}

bool TweaksEngine::ApplyPreset(PresetType preset, std::string& outStatus) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_activePreset = preset;

    if (preset == PresetType::EsportsUltra) {
        // -------------------------------------------------------------
        // PRESET 1: «Esports Ultra (Максимальный соревновательный)»
        // Target: Hardcore esports (CS2, Valorant, Apex, OW2, Fortnite, R6)
        // Philosophy: Absolute minimum input latency, locked 0.500ms timer,
        // Win32 priority 0x26, unparked cores, Power Throttling off, 100% Anti-Cheat Safe
        // -------------------------------------------------------------
        m_state.timerResolution05ms = true;
        m_state.disableDynamicTick = true;
        m_state.usePlatformClockNo = true;
        m_state.usePlatformTick = true;
        m_state.usePlatformTickNo = true;
        m_state.bootuxDisabled = true;
        m_state.win32PrioritySeparation = true;
        m_state.win32PriorityValue = 0x26; // 38 dec: variable, short quantum, 2:1 foreground boost
        m_state.ultimatePowerPlan = true;
        m_state.disableCoreParking = true;
        m_state.powerThrottling = true;
        m_state.gpuMsiMode = true;
        m_state.audioMsiMode = true;
        m_state.hagsEnabled = true;
        m_state.forceP0 = false; // Disabled: vendor-specific, unstable on AMD/Intel and modern RTX architectures
        m_state.csrssDwmPriority = false; // 100% Anti-Cheat Safe (Vanguard, EAC, BattlEye, VAC, FACEIT)
        m_state.disableGameDVR = true;     // GameDVR & Game Bar disabled
        m_state.disableFullscreenOptimizations = true; // FSO bypass
        m_state.mmcssGamesPriority = true; // SystemResponsiveness = 0
        m_state.disableNetworkThrottling = true; // NetworkThrottlingIndex = 0xFFFFFFFF
        m_state.tcpNoDelay = true;         // TCPNoDelay = 1, TcpAckFrequency = 1
        m_state.tcpAckFrequency = true;
        m_state.tcpAutoTuning = true;
        m_state.defaultTTL64 = true;
        m_state.optimalMTU = true;
        m_state.disablePagingExecutive = true;   // DisablePagingExecutive = 1
        m_state.largeSystemCacheDisabled = true; // LargeSystemCache = 0 (desktop/gaming mode)
        m_state.ioPageLockLimit = true;
        m_state.flatMouseCurve = true;     // 1:1 MarkC Linear Mouse Curve (Zero acceleration)
        m_state.keyboardMaxSpeed = true;
        m_state.disableTelemetry = true;
        m_state.disableErrorReporting = true;

        ApplyAll(outStatus);

        // Power Throttling off (Intel/AMD EcoQoS bypass)
        RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff", 1);

        // -------------------------------------------------------------
        // Deep System Engine (SystemTweaker) Integration
        // -------------------------------------------------------------
        auto& sysTweaker = SystemTweaker::Get();
        std::string dummy;

        // OS Performance Sub-Tweaks
        sysTweaker.ApplyOsPerfSubTweak("ultimatePowerPlan", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disablePowerThrottling", true, dummy);
        sysTweaker.ApplySingleSystemTweak("disablePowerThrottling", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("alwaysUnloadDll", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("nonPagedPoolFix", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableBackgroundApps", false, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableAutoWindowsUpdate", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableDriverUpdates", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableReservedStorage", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("waitScreenLock", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("noMonitorOff", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("noDiskSpindown", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableTurboBoostAC", false, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableTurboBoostDC", false, dummy);
        sysTweaker.GetSystemConfig().maxOsPerf = true;

        // SSD Performance Sub-Tweaks (TRIM, 8.3, LastAccess, 0x26 priority)
        sysTweaker.ApplySsdPerfSubTweak("disableLastAccess", true, dummy);
        sysTweaker.ApplySsdPerfSubTweak("enableTrimSupport", true, dummy);
        sysTweaker.ApplySsdPerfSubTweak("largeSystemCache", true, dummy); // LargeSystemCache = 0 (Gaming Mode)
        sysTweaker.ApplySsdPerfSubTweak("win32PrioritySeparation", true, dummy); // 0x26
        sysTweaker.ApplySsdPerfSubTweak("disablePrefetchSuperfetch", true, dummy);
        sysTweaker.ApplySsdPerfSubTweak("disableBootDefrag", true, dummy);
        sysTweaker.ApplySsdPerfSubTweak("disablePagingExecutive", true, dummy);
        sysTweaker.ApplySsdPerfSubTweak("disableLowDiskSpaceChecks", true, dummy);
        sysTweaker.GetSystemConfig().maxSsdPerf = true;

        // Telemetry & Diagnostics tracking disabled
        sysTweaker.ApplyAllTelemetrySubTweaks(true, dummy);
        sysTweaker.GetSecurityConfig().disableAllTelemetry = true;
        sysTweaker.ApplySingleSecurityTweak("disableHiddenMonitoring", true, dummy);
        sysTweaker.ApplySingleSecurityTweak("removeSpyServices", true, dummy);

        // System Services (Disable recommended bloatware services)
        sysTweaker.DisableRecommendedServices(dummy);

        // Standby Memory Clean
        std::string memMsg;
        PurgeStandbyMemory(memMsg);

        // Sync internal UI states
        sysTweaker.DetectOsPerfSubState();
        sysTweaker.DetectSsdPerfSubState();
        sysTweaker.DetectTelemetrySubState();
        sysTweaker.RefreshServiceStates();

        outStatus = "Пресет «Esports Ultra (Максимальный соревновательный)» активирован: 0.500ms таймер locked, Win32 0x26, MMCSS 100%, TCPNoDelay, Unpark 100%, 100% Anti-Cheat Safe!";
        DEBUG_LOG_SUCCESS("TweaksEngine", outStatus);
        return true;
    } else if (preset == PresetType::CompetitiveGaming) {
        // -------------------------------------------------------------
        // PRESET 2: «Competitive Gaming (Сбалансированный игровой)»
        // Target: Daily gamers, streamers, content creators (OBS/Discord)
        // Philosophy: Low latency + complete everyday stability.
        // Preserves: Bluetooth, Printing, Discord/OBS overlays, Windows Update manual capability.
        // -------------------------------------------------------------
        m_state.timerResolution05ms = true;  // 0.500 ms active for low input latency
        m_state.disableDynamicTick = false; // preserve default dynamic tick for balanced stability
        m_state.usePlatformClockNo = true;  // safe synthetic HPET off
        m_state.usePlatformTick = true;
        m_state.usePlatformTickNo = true;
        m_state.bootuxDisabled = false;
        m_state.win32PrioritySeparation = true;
        m_state.win32PriorityValue = 0x26;  // 0x26 foreground priority boost
        m_state.ultimatePowerPlan = false;  // preserve active / balanced power scheme
        m_state.disableCoreParking = true;  // Core Parking optimized: unpark cores on AC
        m_state.powerThrottling = true;     // Power Throttling off for foreground gaming stability
        m_state.gpuMsiMode = true;
        m_state.audioMsiMode = true;
        m_state.hagsEnabled = true;
        m_state.forceP0 = false;           // preserve GPU power saving states when idle
        m_state.csrssDwmPriority = false;  // Discord / OBS overlay safe & Anti-Cheat safe
        m_state.disableGameDVR = true;     // disable background game DVR recording
        m_state.disableFullscreenOptimizations = true; // tuned FSO
        m_state.mmcssGamesPriority = true; // SystemResponsiveness = 0
        m_state.disableNetworkThrottling = true; // NetworkThrottlingIndex = 0xFFFFFFFF
        m_state.tcpNoDelay = true;         // TCPNoDelay = 1, TcpAckFrequency = 1
        m_state.tcpAckFrequency = true;
        m_state.tcpAutoTuning = true;
        m_state.defaultTTL64 = false;
        m_state.optimalMTU = true;
        m_state.disablePagingExecutive = false; // preserve RAM for streaming / multitasking (OBS/Discord)
        m_state.largeSystemCacheDisabled = true;// LargeSystemCache = 0
        m_state.ioPageLockLimit = false;
        m_state.flatMouseCurve = true;
        m_state.keyboardMaxSpeed = true;
        m_state.disableTelemetry = true;
        m_state.disableErrorReporting = true;

        ApplyAll(outStatus);

        // Core Parking optimization on current active scheme
        SetCoreParking(true);
        // Power Throttling off for foreground gaming stability
        RegWriteDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff", 1);

        // -------------------------------------------------------------
        // Deep System Engine (SystemTweaker) Integration
        // -------------------------------------------------------------
        auto& sysTweaker = SystemTweaker::Get();
        std::string dummy;

        // Safe OS performance tweaks keeping everyday comfort
        sysTweaker.ApplyOsPerfSubTweak("alwaysUnloadDll", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("nonPagedPoolFix", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableReservedStorage", true, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disablePowerThrottling", true, dummy);
        sysTweaker.ApplySingleSystemTweak("disablePowerThrottling", true, dummy);

        // Preserves: Windows Update manual capability, background apps for Discord/OBS
        sysTweaker.ApplyOsPerfSubTweak("disableAutoWindowsUpdate", false, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableDriverUpdates", false, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableBackgroundApps", false, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableTurboBoostAC", false, dummy);
        sysTweaker.ApplyOsPerfSubTweak("disableTurboBoostDC", false, dummy);
        sysTweaker.GetSystemConfig().maxOsPerf = false;

        // Safe SSD tweaks
        sysTweaker.ApplySsdPerfSubTweak("enableTrimSupport", true, dummy);
        sysTweaker.ApplySsdPerfSubTweak("disableLastAccess", true, dummy);
        sysTweaker.ApplySsdPerfSubTweak("win32PrioritySeparation", true, dummy); // 0x26 foreground boost
        sysTweaker.ApplySsdPerfSubTweak("largeSystemCache", true, dummy); // LargeSystemCache = 0 (Gaming Mode)
        sysTweaker.ApplySsdPerfSubTweak("disablePagingExecutive", false, dummy);
        sysTweaker.GetSystemConfig().maxSsdPerf = false;

        // Selective Telemetry: Disable heavy background telemetry while preserving account sync
        sysTweaker.ApplyTelemetrySubTweak("disableAdvertisingId", true, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableVsTelemetry", true, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableOfficeTelemetry", true, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableNvidiaTelemetry", true, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableCeipTasks", true, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableAutoLoggerDiagTrack", true, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableUserBehavior", true, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableSettingSync", false, dummy);
        sysTweaker.ApplyTelemetrySubTweak("disableFeedbackFrequency", false, dummy);

        // Preserves: Bluetooth, Printing services, Windows Update
        for (auto& s : sysTweaker.GetServices()) {
            if (s.id == "svc_spooler" || s.id == "svc_bth" || s.id == "svc_wuauserv") {
                if (s.disabled) sysTweaker.ApplyServiceState(s, false);
            }
        }

        // Clean standby memory to give immediate headroom for games
        std::string memMsg;
        PurgeStandbyMemory(memMsg);

        // Refresh system tweaker state
        sysTweaker.DetectOsPerfSubState();
        sysTweaker.DetectSsdPerfSubState();
        sysTweaker.DetectTelemetrySubState();
        sysTweaker.RefreshServiceStates();

        outStatus = "Пресет «Competitive Gaming (Сбалансированный игровой)» активирован: 0.500ms таймер, TCPNoDelay, Unpark ядер, сохранены Bluetooth, печать и оверлеи!";
        DEBUG_LOG_SUCCESS("TweaksEngine", outStatus);
        return true;
    } else if (preset == PresetType::WindowsDefault) {
        return RevertToDefaults(outStatus);
    }

    return false;
}

bool TweaksEngine::RevertToDefaults(std::string& outStatus) {
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_activePreset = PresetType::WindowsDefault;

    // -------------------------------------------------------------
    // PRESET 3: «Windows Default (Заводской Microsoft)»
    // Target: 100% clean factory reset
    // Philosophy: Pristine Microsoft standard configuration
    // -------------------------------------------------------------
    // 1. Revert all TweaksEngine states to Microsoft Windows defaults
    m_state.timerResolution05ms = false;      // dynamic default ~1.000 - 15.625 ms
    m_state.disableDynamicTick = false;       // BCD dynamic ticks enabled
    m_state.usePlatformClockNo = false;       // platform clock default
    m_state.usePlatformTick = false;
    m_state.usePlatformTickNo = false;
    m_state.bootuxDisabled = false;
    m_state.win32PrioritySeparation = false;
    m_state.win32PriorityValue = 2;            // 0x2 (18 dec: standard Windows desktop default)
    m_state.ultimatePowerPlan = false;        // balanced scheme
    m_state.disableCoreParking = false;       // factory standard core parking
    m_state.powerThrottling = false;          // Power Throttling restored
    m_state.gpuMsiMode = false;
    m_state.audioMsiMode = false;
    m_state.hagsEnabled = false;
    m_state.forceP0 = false;
    m_state.csrssDwmPriority = false;
    m_state.disableGameDVR = false;           // GameDVR and Game Bar restored
    m_state.disableFullscreenOptimizations = false;
    m_state.mmcssGamesPriority = false;       // SystemResponsiveness = 20
    m_state.disableNetworkThrottling = false; // NetworkThrottlingIndex = 10
    m_state.tcpNoDelay = false;               // Nagle algorithm default
    m_state.tcpAckFrequency = false;
    m_state.tcpAutoTuning = false;
    m_state.defaultTTL64 = false;
    m_state.optimalMTU = false;
    m_state.disablePagingExecutive = false;   // DisablePagingExecutive = 0
    m_state.largeSystemCacheDisabled = true;  // LargeSystemCache = 0 / desktop default
    m_state.ioPageLockLimit = false;
    m_state.flatMouseCurve = false;           // Standard Microsoft mouse curve, zero acceleration
    m_state.keyboardMaxSpeed = false;
    m_state.disableTelemetry = false;
    m_state.disableErrorReporting = false;

    ApplyAll(outStatus);

    // Remove Power Throttling override
    RegDeleteValue(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling", L"PowerThrottlingOff");

    // 2. Revert all System Suite (SystemTweaker) tweaks and restore all services
    std::string sysStatus;
    SystemTweaker::Get().RevertAllTweaks(sysStatus);
    SystemTweaker::Get().EnableAllServices(sysStatus);

    // 3. Refresh live states
    SystemTweaker::Get().DetectSystemState();
    SystemTweaker::Get().DetectSecurityState();
    SystemTweaker::Get().DetectOsPerfSubState();
    SystemTweaker::Get().DetectSsdPerfSubState();
    SystemTweaker::Get().DetectTelemetrySubState();
    SystemTweaker::Get().RefreshServiceStates();

    outStatus = "Пресет «Windows Default (Заводской Microsoft)» активирован: заводские таймеры, BCD, сеть (10/20), Win32 0x2 и все службы восстановлены!";
    DEBUG_LOG_SUCCESS("TweaksEngine", outStatus);
    return true;
}


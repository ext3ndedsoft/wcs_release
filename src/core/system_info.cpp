#include "system_info.h"
#include "common_utils.h"
#include <windows.h>
#include <dxgi.h>
#include <powrprof.h>
#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "pdh.lib")

// NTDLL function prototypes
typedef NTSTATUS(NTAPI* pfnNtQueryTimerResolution)(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);

SystemInfo& SystemInfo::Get() {
    static SystemInfo instance;
    return instance;
}

SystemInfo::SystemInfo() {
    InitFast();
    InitGpuPdh();
}

SystemInfo::~SystemInfo() {
    CloseGpuPdh();
}

void SystemInfo::InitFast() {
    std::lock_guard<std::mutex> lock(m_mutex);
    DetectCPU();
    DetectGPUsFast();
    DetectMemory();
    DetectOS();
    DetectPowerPlan();
    DetectTimerResolution();
}

void SystemInfo::Refresh() {
    InitFast();
    DetectGPUsDetailed();
    m_isInitialized.store(true);
}

void SystemInfo::RefreshFullAsync(std::function<void()> onComplete) {
    std::thread([this, onComplete]() {
        DetectGPUsDetailed();
        m_isInitialized.store(true);
        if (onComplete) {
            onComplete();
        }
    }).detach();
}

SystemSpecs SystemInfo::GetSpecs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_specs;
}

void SystemInfo::DetectCPU() {
    // 1. Core count
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    m_specs.logicalCores = sysInfo.dwNumberOfProcessors;

    // 2. CPU brand string via CPUID
    int cpuInfo[4] = { 0 };
    __cpuid(cpuInfo, 0x80000000);
    unsigned int nExIds = cpuInfo[0];

    char brand[0x40] = { 0 };
    if (nExIds >= 0x80000004) {
        __cpuid(reinterpret_cast<int*>(brand), 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand + 16), 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand + 32), 0x80000004);
        m_specs.cpuName = brand;
        // Trim leading spaces
        size_t firstNonSpace = m_specs.cpuName.find_first_not_of(' ');
        if (firstNonSpace != std::string::npos) {
            m_specs.cpuName = m_specs.cpuName.substr(firstNonSpace);
        }
    } else {
        m_specs.cpuName = "Generic x64 Processor";
    }
}

void SystemInfo::DetectGPUsFast() {
    m_specs.gpus.clear();
    m_specs.primaryGPUName = "GPU Initializing...";
    m_specs.primaryGPUMSIEnabled = true;

    // Fast DXGI adapter enumeration without slow PCI registry recursion
    IDXGIFactory1* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
        IDXGIAdapter1* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            pAdapter->GetDesc1(&desc);

            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && desc.DedicatedVideoMemory > 0) {
                GPUInfo info;
                info.name = desc.Description;
                info.dedicatedVRAM = desc.DedicatedVideoMemory;

                char hexVen[16], hexDev[16];
                snprintf(hexVen, sizeof(hexVen), "VEN_%04X", desc.VendorId);
                snprintf(hexDev, sizeof(hexDev), "DEV_%04X", desc.DeviceId);

                bool msiFound = false;
                bool msiOn = false;
                ScopedHKey hPciKey;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, hPciKey.GetAddressOf()) == ERROR_SUCCESS) {
                    DWORD subKeys = 0;
                    RegQueryInfoKeyW(hPciKey.Get(), NULL, NULL, NULL, &subKeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                    for (DWORD k = 0; k < subKeys; ++k) {
                        wchar_t devKeyName[256];
                        DWORD devKeyLen = 256;
                        if (RegEnumKeyExW(hPciKey.Get(), k, devKeyName, &devKeyLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                            std::wstring wDevKey = devKeyName;
                            std::string sDevKey = WideStrToUtf8(wDevKey);
                            std::transform(sDevKey.begin(), sDevKey.end(), sDevKey.begin(), ::toupper);
                            if (sDevKey.find(hexVen) != std::string::npos && sDevKey.find(hexDev) != std::string::npos) {
                                ScopedHKey hSubDev;
                                if (RegOpenKeyExW(hPciKey.Get(), devKeyName, 0, KEY_READ, hSubDev.GetAddressOf()) == ERROR_SUCCESS) {
                                    DWORD instCount = 0;
                                    RegQueryInfoKeyW(hSubDev.Get(), NULL, NULL, NULL, &instCount, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                                    for (DWORD inst = 0; inst < instCount; ++inst) {
                                        wchar_t instName[256];
                                        DWORD instLen = 256;
                                        if (RegEnumKeyExW(hSubDev.Get(), inst, instName, &instLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                                            std::wstring msiPath = std::wstring(devKeyName) + L"\\" + instName + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
                                            ScopedHKey hMsi;
                                            if (RegOpenKeyExW(hPciKey.Get(), msiPath.c_str(), 0, KEY_READ, hMsi.GetAddressOf()) == ERROR_SUCCESS) {
                                                DWORD msiVal = 0;
                                                DWORD dwSize = sizeof(DWORD);
                                                if (RegQueryValueExW(hMsi.Get(), L"MSISupported", NULL, NULL, (LPBYTE)&msiVal, &dwSize) == ERROR_SUCCESS) {
                                                    msiFound = true;
                                                    msiOn = (msiVal == 1);
                                                }
                                                break;
                                            }
                                        }
                                    }
                                    if (msiFound) break;
                                }
                            }
                        }
                    }
                }
                info.msiSupported = msiFound;
                info.msiEnabled = msiOn;
                m_specs.gpus.push_back(info);
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }

    if (!m_specs.gpus.empty()) {
        m_specs.primaryGPUName = WideToUtf8(m_specs.gpus[0].name);
        m_specs.primaryGPUMSIEnabled = m_specs.gpus[0].msiEnabled;
    } else {
        m_specs.primaryGPUName = "Discrete GPU";
        m_specs.primaryGPUMSIEnabled = false;
    }
}

void SystemInfo::DetectGPUsDetailed() {
    std::vector<GPUInfo> detectedGpus;
    std::string primaryName = "No Discrete GPU Detected";
    bool primaryMsi = false;

    // 1. Query DXGI for GPU names and VRAM
    IDXGIFactory1* pFactory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
        IDXGIAdapter1* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            pAdapter->GetDesc1(&desc);

            // Skip software / Microsoft Basic Render Driver
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && desc.DedicatedVideoMemory > 0) {
                GPUInfo info;
                info.name = desc.Description;
                info.dedicatedVRAM = desc.DedicatedVideoMemory;

                char hexVen[16], hexDev[16];
                snprintf(hexVen, sizeof(hexVen), "VEN_%04X", desc.VendorId);
                snprintf(hexDev, sizeof(hexDev), "DEV_%04X", desc.DeviceId);

                // Scan HKLM\SYSTEM\CurrentControlSet\Enum\PCI
                ScopedHKey hPciKey;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\PCI", 0, KEY_READ, hPciKey.GetAddressOf()) == ERROR_SUCCESS) {
                    DWORD subKeys = 0;
                    RegQueryInfoKeyW(hPciKey.Get(), NULL, NULL, NULL, &subKeys, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

                    for (DWORD k = 0; k < subKeys; ++k) {
                        wchar_t devKeyName[256];
                        DWORD devKeyLen = 256;
                        if (RegEnumKeyExW(hPciKey.Get(), k, devKeyName, &devKeyLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                            std::wstring wDevKey = devKeyName;
                            std::string sDevKey = WideStrToUtf8(wDevKey);
                            std::transform(sDevKey.begin(), sDevKey.end(), sDevKey.begin(), ::toupper);

                            if (sDevKey.find(hexVen) != std::string::npos && sDevKey.find(hexDev) != std::string::npos) {
                                ScopedHKey hSubDev;
                                if (RegOpenKeyExW(hPciKey.Get(), devKeyName, 0, KEY_READ, hSubDev.GetAddressOf()) == ERROR_SUCCESS) {
                                    DWORD instCount = 0;
                                    RegQueryInfoKeyW(hSubDev.Get(), NULL, NULL, NULL, &instCount, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                                    for (DWORD inst = 0; inst < instCount; ++inst) {
                                        wchar_t instName[256];
                                        DWORD instLen = 256;
                                        if (RegEnumKeyExW(hSubDev.Get(), inst, instName, &instLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                                            std::wstring msiPath = std::wstring(devKeyName) + L"\\" + instName + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
                                            info.pciDevicePath = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\") + devKeyName + L"\\" + instName;

                                            ScopedHKey hMsi;
                                            if (RegOpenKeyExW(hPciKey.Get(), msiPath.c_str(), 0, KEY_READ, hMsi.GetAddressOf()) == ERROR_SUCCESS) {
                                                DWORD msiVal = 0;
                                                DWORD dwSize = sizeof(DWORD);
                                                if (RegQueryValueExW(hMsi.Get(), L"MSISupported", NULL, NULL, (LPBYTE)&msiVal, &dwSize) == ERROR_SUCCESS) {
                                                    info.msiSupported = true;
                                                    info.msiEnabled = (msiVal == 1);
                                                }
                                            }

                                            // Check Priority
                                            std::wstring affPath = std::wstring(devKeyName) + L"\\" + instName + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";
                                            ScopedHKey hAff;
                                            if (RegOpenKeyExW(hPciKey.Get(), affPath.c_str(), 0, KEY_READ, hAff.GetAddressOf()) == ERROR_SUCCESS) {
                                                DWORD prioVal = 0;
                                                DWORD dwSize = sizeof(DWORD);
                                                if (RegQueryValueExW(hAff.Get(), L"DevicePriority", NULL, NULL, (LPBYTE)&prioVal, &dwSize) == ERROR_SUCCESS) {
                                                    info.msiPriority = (int)prioVal;
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }

                detectedGpus.push_back(info);
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }

    if (!detectedGpus.empty()) {
        primaryName = WideToUtf8(detectedGpus[0].name);
        primaryMsi = detectedGpus[0].msiEnabled;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_specs.gpus = detectedGpus;
    m_specs.primaryGPUName = primaryName;
    m_specs.primaryGPUMSIEnabled = primaryMsi;
}

void SystemInfo::DetectMemory() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        m_specs.totalRAM_GB = (double)memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
        m_specs.freeRAM_GB  = (double)memInfo.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
        m_specs.standbyRAM_GB = m_specs.totalRAM_GB * 0.18; // baseline estimation for UI display
    }
}

void SystemInfo::RefreshMemoryStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        m_specs.freeRAM_GB = (double)memInfo.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
    }
}

void SystemInfo::DetectOS() {
    m_specs.osName = "Windows 10 / 11 Pro";
    m_specs.osBuild = "Build 22631";

    ScopedHKey hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, hKey.GetAddressOf()) == ERROR_SUCCESS) {
        wchar_t prodName[128] = { 0 };
        DWORD dwSize = sizeof(prodName);
        if (RegQueryValueExW(hKey.Get(), L"ProductName", NULL, NULL, (LPBYTE)prodName, &dwSize) == ERROR_SUCCESS) {
            m_specs.osName = WideToUtf8(prodName);
        }

        wchar_t displayVer[64] = { 0 };
        dwSize = sizeof(displayVer);
        if (RegQueryValueExW(hKey.Get(), L"DisplayVersion", NULL, NULL, (LPBYTE)displayVer, &dwSize) == ERROR_SUCCESS) {
            m_specs.osBuild = WideToUtf8(displayVer);
        }

        wchar_t currentBuild[64] = { 0 };
        dwSize = sizeof(currentBuild);
        if (RegQueryValueExW(hKey.Get(), L"CurrentBuildNumber", NULL, NULL, (LPBYTE)currentBuild, &dwSize) == ERROR_SUCCESS) {
            m_specs.osBuild += " (" + WideToUtf8(currentBuild) + ")";
        }
    }
}

void SystemInfo::DetectPowerPlan() {
    m_specs.activePowerPlan = "Сбалансированная (Balanced)";
    m_specs.isUltimatePerformanceActive = false;

    static const GUID GUID_ULTIMATE_PERF = { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61 } };
    static const GUID GUID_HIGH_PERF     = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
    static const GUID GUID_BALANCED      = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
    static const GUID GUID_POWER_SAVER   = { 0xa1841308, 0x3541, 0x4fab, { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a } };

    GUID* pActiveScheme = nullptr;
    if (PowerGetActiveScheme(NULL, &pActiveScheme) == ERROR_SUCCESS && pActiveScheme != nullptr) {
        if (IsEqualGUID(*pActiveScheme, GUID_ULTIMATE_PERF)) {
            m_specs.activePowerPlan = "Максимальная производительность (Ultimate Performance)";
            m_specs.isUltimatePerformanceActive = true;
        } else if (IsEqualGUID(*pActiveScheme, GUID_HIGH_PERF)) {
            m_specs.activePowerPlan = "Высокая производительность (High Performance)";
            m_specs.isUltimatePerformanceActive = false;
        } else if (IsEqualGUID(*pActiveScheme, GUID_BALANCED)) {
            m_specs.activePowerPlan = "Сбалансированная (Balanced)";
            m_specs.isUltimatePerformanceActive = false;
        } else if (IsEqualGUID(*pActiveScheme, GUID_POWER_SAVER)) {
            m_specs.activePowerPlan = "Экономия энергии (Power Saver)";
            m_specs.isUltimatePerformanceActive = false;
        } else {
            UCHAR buffer[512] = { 0 };
            DWORD bufferSize = sizeof(buffer);
            if (PowerReadFriendlyName(NULL, pActiveScheme, NULL, NULL, buffer, &bufferSize) == ERROR_SUCCESS) {
                wchar_t* wName = reinterpret_cast<wchar_t*>(buffer);
                std::string name = WideToUtf8(wName);
                if (!name.empty()) {
                    m_specs.activePowerPlan = name;
                }
            }

            if (m_specs.activePowerPlan.find("Ultimate") != std::string::npos ||
                m_specs.activePowerPlan.find("Максимальная") != std::string::npos ||
                m_specs.activePowerPlan.find("Esports") != std::string::npos) {
                m_specs.isUltimatePerformanceActive = true;
            }
        }
        LocalFree(pActiveScheme);
    }
}

void SystemInfo::DetectTimerResolution() {
    static HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    static auto pfnQuery = hNtDll ? (pfnNtQueryTimerResolution)GetProcAddress(hNtDll, "NtQueryTimerResolution") : nullptr;
    if (pfnQuery) {
        ULONG minRes = 0, maxRes = 0, curRes = 0;
        if (pfnQuery(&minRes, &maxRes, &curRes) >= 0) {
            // Values in 100ns units -> convert to ms (10000 units = 1 ms)
            m_specs.minTimerResolutionMs = (float)minRes / 10000.0f;
            m_specs.maxTimerResolutionMs = (float)maxRes / 10000.0f;
            m_specs.currentTimerResolutionMs = (float)curRes / 10000.0f;
        }
    }
}

float SystemInfo::QueryLiveTimerResolution() {
    static ULONGLONG s_lastQuery = 0;
    static float s_cachedRes = 0.5f;
    ULONGLONG now = GetTickCount64();
    if (s_lastQuery != 0 && (now - s_lastQuery) < 150) {
        return s_cachedRes;
    }
    s_lastQuery = now;

    std::lock_guard<std::mutex> lock(m_mutex);
    DetectTimerResolution();
    s_cachedRes = m_specs.currentTimerResolutionMs;
    return s_cachedRes;
}

void SystemInfo::InitGpuPdh() {
    if (m_hGpuQuery) {
        return;
    }
    PDH_HQUERY hQuery = nullptr;
    PDH_HCOUNTER hCounter = nullptr;

    if (PdhOpenQueryW(nullptr, 0, &hQuery) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(hQuery, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &hCounter) == ERROR_SUCCESS) {
            PdhCollectQueryData(hQuery); // Prime the counter
            m_hGpuQuery = hQuery;
            m_hGpuCounter = hCounter;
        } else {
            PdhCloseQuery(hQuery);
        }
    }
}

void SystemInfo::CloseGpuPdh() {
    if (m_hGpuQuery) {
        PdhCloseQuery(reinterpret_cast<PDH_HQUERY>(m_hGpuQuery));
        m_hGpuQuery = nullptr;
        m_hGpuCounter = nullptr;
    }
}

float SystemInfo::QueryLiveCpuUsage() {
    std::lock_guard<std::mutex> lock(m_mutex);

    uint64_t now = GetTickCount64();
    if (m_lastCpuQueryTick != 0 && (now - m_lastCpuQueryTick) < 250) {
        return m_cachedCpuUsage;
    }

    FILETIME idleTime = { 0 }, kernelTime = { 0 }, userTime = { 0 };
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return m_cachedCpuUsage;
    }

    auto FileTimeToUInt64 = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    uint64_t idle = FileTimeToUInt64(idleTime);
    uint64_t kernel = FileTimeToUInt64(kernelTime);
    uint64_t user = FileTimeToUInt64(userTime);

    if (m_lastCpuQueryTick == 0) {
        m_prevIdleTime = idle;
        m_prevKernelTime = kernel;
        m_prevUserTime = user;
        m_lastCpuQueryTick = now;
        return 0.0f;
    }

    uint64_t diffIdle = (idle >= m_prevIdleTime) ? (idle - m_prevIdleTime) : 0;
    uint64_t diffKernel = (kernel >= m_prevKernelTime) ? (kernel - m_prevKernelTime) : 0;
    uint64_t diffUser = (user >= m_prevUserTime) ? (user - m_prevUserTime) : 0;

    uint64_t totalSystem = diffKernel + diffUser; // In Windows, kernel time includes idle
    if (totalSystem > 0) {
        uint64_t totalUsage = (totalSystem >= diffIdle) ? (totalSystem - diffIdle) : 0;
        float percent = static_cast<float>((static_cast<double>(totalUsage) / static_cast<double>(totalSystem)) * 100.0);
        if (percent < 0.0f) percent = 0.0f;
        if (percent > 100.0f) percent = 100.0f;
        m_cachedCpuUsage = percent;
    }

    m_prevIdleTime = idle;
    m_prevKernelTime = kernel;
    m_prevUserTime = user;
    m_lastCpuQueryTick = now;
    return m_cachedCpuUsage;
}

float SystemInfo::QueryLiveGpuUsage() {
    std::lock_guard<std::mutex> lock(m_mutex);

    uint64_t now = GetTickCount64();
    if (m_lastGpuQueryTick != 0 && (now - m_lastGpuQueryTick) < 400) {
        return m_cachedGpuUsage;
    }

    if (!m_hGpuQuery) {
        InitGpuPdh();
        if (!m_hGpuQuery) {
            return 0.0f;
        }
    }

    PDH_HQUERY hQuery = reinterpret_cast<PDH_HQUERY>(m_hGpuQuery);
    PDH_HCOUNTER hCounter = reinterpret_cast<PDH_HCOUNTER>(m_hGpuCounter);

    PDH_STATUS status = PdhCollectQueryData(hQuery);
    if (status != ERROR_SUCCESS) {
        CloseGpuPdh();
        return m_cachedGpuUsage;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    status = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
    if (status == PDH_MORE_DATA && bufferSize > 0) {
        std::vector<BYTE> buffer(bufferSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        status = PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
        if (status == ERROR_SUCCESS) {
            double totalLoad = 0.0;
            for (DWORD i = 0; i < itemCount; ++i) {
                if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                    items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
                    totalLoad += items[i].FmtValue.doubleValue;
                }
            }
            if (totalLoad < 0.0) totalLoad = 0.0;
            if (totalLoad > 100.0) totalLoad = 100.0;
            m_cachedGpuUsage = static_cast<float>(totalLoad);
        }
    }

    m_lastGpuQueryTick = now;
    return m_cachedGpuUsage;
}


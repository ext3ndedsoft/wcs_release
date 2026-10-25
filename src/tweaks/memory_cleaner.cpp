#include "memory_cleaner.h"
#include "config_manager.h"
#include "common_utils.h"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <algorithm>

typedef LONG NTSTATUS;
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

typedef enum _SYSTEM_MEMORY_LIST_COMMAND {
    MemoryCaptureAccessedBits = 0,
    MemoryCaptureAndResetAccessedBits = 1,
    MemoryEmptyWorkingSets = 2,
    MemoryFlushModifiedList = 3,
    MemoryPurgeStandbyList = 4,
    MemoryPurgeLowPriorityStandbyList = 5,
    MemoryCommandMax = 6
} SYSTEM_MEMORY_LIST_COMMAND;

typedef NTSTATUS (NTAPI *pfnNtSetSystemInformation)(
    INT SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength
);

MemoryCleaner& MemoryCleaner::Get() {
    static MemoryCleaner instance;
    return instance;
}

MemoryCleaner::MemoryCleaner() {
    EnablePrivilege(L"SeIncreaseQuotaPrivilege");
    EnablePrivilege(L"SeProfileSingleProcessPrivilege");
    QueryDetailedStats(); // Initialize baseline peak stats
    ResetIntervalTimer();
}

bool MemoryCleaner::EnablePrivilege(const wchar_t* privName) {
    HANDLE rawToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) {
        return false;
    }
    ScopedHandle hToken(rawToken);

    LUID luid;
    if (!LookupPrivilegeValueW(NULL, privName, &luid)) {
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL ok = AdjustTokenPrivileges(hToken.Get(), FALSE, &tp, sizeof(tp), NULL, NULL);
    return (ok && GetLastError() == ERROR_SUCCESS);
}

bool MemoryCleaner::PurgeStandbyList() {
    EnablePrivilege(L"SeIncreaseQuotaPrivilege");
    EnablePrivilege(L"SeProfileSingleProcessPrivilege");

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) return false;

    pfnNtSetSystemInformation NtSetSystemInfo = (pfnNtSetSystemInformation)GetProcAddress(hNtdll, "NtSetSystemInformation");
    if (!NtSetSystemInfo) return false;

    SYSTEM_MEMORY_LIST_COMMAND cmdStandby = MemoryPurgeStandbyList;
    NTSTATUS status1 = NtSetSystemInfo(80 /* SystemMemoryListInformation */, &cmdStandby, sizeof(cmdStandby));

    SYSTEM_MEMORY_LIST_COMMAND cmdLow = MemoryPurgeLowPriorityStandbyList;
    NtSetSystemInfo(80, &cmdLow, sizeof(cmdLow));

    SYSTEM_MEMORY_LIST_COMMAND cmdModified = MemoryFlushModifiedList;
    NtSetSystemInfo(80, &cmdModified, sizeof(cmdModified));

    return (status1 == STATUS_SUCCESS);
}

bool MemoryCleaner::EmptyProcessWorkingSets() {
    ScopedHandle hSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!hSnap.IsValid()) return false;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap.Get(), &pe)) {
        do {
            if (pe.th32ProcessID <= 4) continue; // Skip System and Idle
            ScopedHandle hProcess(OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID));
            if (hProcess.IsValid()) {
                SetProcessWorkingSetSize(hProcess.Get(), (SIZE_T)-1, (SIZE_T)-1);
            }
        } while (Process32NextW(hSnap.Get(), &pe));
    }
    return true;
}

bool MemoryCleaner::TrimProcessesWorkingSet(size_t& outBytesFreed, std::string& outStatus) {
    MEMORYSTATUSEX memBefore = { sizeof(memBefore) };
    GlobalMemoryStatusEx(&memBefore);

    EnablePrivilege(L"SeIncreaseQuotaPrivilege");
    EmptyProcessWorkingSets();

    MEMORYSTATUSEX memAfter = { sizeof(memAfter) };
    GlobalMemoryStatusEx(&memAfter);

    if (memAfter.ullAvailPhys > memBefore.ullAvailPhys) {
        outBytesFreed = (size_t)(memAfter.ullAvailPhys - memBefore.ullAvailPhys);
    } else {
        outBytesFreed = 0;
    }

    m_lastFreedBytes = outBytesFreed;
    m_totalSessionFreedBytes += outBytesFreed;

    char buf[128];
    if (outBytesFreed > 0) {
        snprintf(buf, sizeof(buf), "Рабочие наборы процессов сброшены");
    } else {
        snprintf(buf, sizeof(buf), "Рабочие наборы всех процессов оптимизированы");
    }
    outStatus = buf;
    m_lastActionStatus = outStatus;
    QueryDetailedStats(true);
    return true;
}

bool MemoryCleaner::ClearSystemCache(size_t& outBytesFreed, std::string& outStatus) {
    MEMORYSTATUSEX memBefore = { sizeof(memBefore) };
    GlobalMemoryStatusEx(&memBefore);

    EnablePrivilege(L"SeIncreaseQuotaPrivilege");
    EnablePrivilege(L"SeProfileSingleProcessPrivilege");
    PurgeStandbyList();

    MEMORYSTATUSEX memAfter = { sizeof(memAfter) };
    GlobalMemoryStatusEx(&memAfter);

    if (memAfter.ullAvailPhys > memBefore.ullAvailPhys) {
        outBytesFreed = (size_t)(memAfter.ullAvailPhys - memBefore.ullAvailPhys);
    } else {
        outBytesFreed = 0;
    }

    m_lastFreedBytes = outBytesFreed;
    m_totalSessionFreedBytes += outBytesFreed;

    char buf[128];
    if (outBytesFreed > 0) {
        snprintf(buf, sizeof(buf), "Системный кэш Standby очищен");
    } else {
        snprintf(buf, sizeof(buf), "Системный кэш Standby сброшен");
    }
    outStatus = buf;
    m_lastActionStatus = outStatus;
    QueryDetailedStats(true);
    return true;
}

bool MemoryCleaner::CleanAll(size_t& outBytesFreed, std::string& outStatus) {
    MEMORYSTATUSEX memBefore = { sizeof(memBefore) };
    GlobalMemoryStatusEx(&memBefore);

    EnablePrivilege(L"SeIncreaseQuotaPrivilege");
    EnablePrivilege(L"SeProfileSingleProcessPrivilege");

    EmptyProcessWorkingSets();
    PurgeStandbyList();

    MEMORYSTATUSEX memAfter = { sizeof(memAfter) };
    GlobalMemoryStatusEx(&memAfter);

    if (memAfter.ullAvailPhys > memBefore.ullAvailPhys) {
        outBytesFreed = (size_t)(memAfter.ullAvailPhys - memBefore.ullAvailPhys);
    } else {
        outBytesFreed = 0;
    }

    m_lastFreedBytes = outBytesFreed;
    m_totalSessionFreedBytes += outBytesFreed;

    char buf[128];
    if (outBytesFreed > 0) {
        snprintf(buf, sizeof(buf), "Полная оптимизация памяти выполнена");
    } else {
        snprintf(buf, sizeof(buf), "ОЗУ и кэш в оптимальном состоянии");
    }
    outStatus = buf;
    m_lastActionStatus = outStatus;
    QueryDetailedStats(true);
    ResetIntervalTimer();
    return true;
}

bool MemoryCleaner::CleanRAM() {
    size_t freed = 0;
    std::string s;
    return CleanAll(freed, s);
}

bool MemoryCleaner::CleanRAM(size_t& outBytesFreed, std::string& outStatus) {
    return CleanAll(outBytesFreed, outStatus);
}

DetailedMemoryStats MemoryCleaner::QueryDetailedStats(bool forceFresh) {
    static DetailedMemoryStats s_cachedStats;
    static ULONGLONG s_lastQueryTick = 0;
    ULONGLONG now = GetTickCount64();

    if (!forceFresh && s_lastQueryTick != 0 && (now - s_lastQueryTick) < 150) {
        return s_cachedStats;
    }
    s_lastQueryTick = now;

    DetailedMemoryStats s;

    MEMORYSTATUSEX mem = { sizeof(mem) };
    if (GlobalMemoryStatusEx(&mem)) {
        s.totalRAM_GB = (double)mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
        s.freeRAM_GB = (double)mem.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
        s.usedRAM_GB = (s.totalRAM_GB > s.freeRAM_GB) ? (s.totalRAM_GB - s.freeRAM_GB) : 0.0;
        s.usedPercent = (s.totalRAM_GB > 0.0) ? (float)(s.usedRAM_GB / s.totalRAM_GB * 100.0f) : 0.0f;

        s.pagefileTotal_GB = (double)mem.ullTotalPageFile / (1024.0 * 1024.0 * 1024.0);
        double pfAvail = (double)mem.ullAvailPageFile / (1024.0 * 1024.0 * 1024.0);
        s.pagefileUsed_GB = (s.pagefileTotal_GB > pfAvail) ? (s.pagefileTotal_GB - pfAvail) : 0.0;
        s.pagefilePercent = (s.pagefileTotal_GB > 0.0) ? (float)(s.pagefileUsed_GB / s.pagefileTotal_GB * 100.0f) : 0.0f;

        if (s.usedPercent > m_peakUsagePercent) {
            m_peakUsagePercent = s.usedPercent;
        }
        s.peakMemoryPercent = m_peakUsagePercent;
    }

    PERFORMANCE_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        double pageSizeBytes = (double)pi.PageSize;
        s.standbyRAM_GB = ((double)pi.SystemCache * pageSizeBytes) / (1024.0 * 1024.0 * 1024.0);
    } else {
        s.standbyRAM_GB = s.totalRAM_GB * 0.15; // Safe baseline fallback
    }

    s_cachedStats = s;
    return s;
}

void MemoryCleaner::ResetIntervalTimer() {
    const auto& cfg = ConfigManager::Get().GetConfig();
    int intervalMin = cfg.ramCleanIntervalMinutes;
    if (intervalMin < 1) intervalMin = 15;
    m_lastKnownIntervalMinutes = intervalMin;

    unsigned long long now = GetTickCount64();
    unsigned long long intervalMs = (unsigned long long)intervalMin * 60ULL * 1000ULL;
    m_lastCleanTick = now;
    m_nextCleanTick = now + intervalMs;
}

int MemoryCleaner::GetRemainingSeconds() const {
    const auto& cfg = ConfigManager::Get().GetConfig();
    if (!cfg.trimWorkingSetOnInterval && !cfg.clearCacheOnInterval) return 0;

    unsigned long long now = GetTickCount64();
    if (m_nextCleanTick == 0 || now >= m_nextCleanTick) return 0;
    return (int)((m_nextCleanTick - now + 999ULL) / 1000ULL);
}

float MemoryCleaner::GetTimerProgress() const {
    const auto& cfg = ConfigManager::Get().GetConfig();
    if (!cfg.trimWorkingSetOnInterval && !cfg.clearCacheOnInterval) return 0.0f;

    int intervalMin = cfg.ramCleanIntervalMinutes;
    if (intervalMin < 1) intervalMin = 15;
    unsigned long long intervalMs = (unsigned long long)intervalMin * 60ULL * 1000ULL;
    if (intervalMs == 0) return 0.0f;

    unsigned long long now = GetTickCount64();
    if (now >= m_nextCleanTick) return 1.0f;
    if (now <= m_lastCleanTick) return 0.0f;

    float p = (float)(now - m_lastCleanTick) / (float)intervalMs;
    return (std::clamp)(p, 0.0f, 1.0f);
}

void MemoryCleaner::Update(float /*dt*/) {
    const auto& cfg = ConfigManager::Get().GetConfig();
    if (!cfg.autoRamCleanEnabled) {
        return;
    }

    unsigned long long now = GetTickCount64();

    // Auto-resync if user changed interval setting
    if (cfg.ramCleanIntervalMinutes != m_lastKnownIntervalMinutes) {
        ResetIntervalTimer();
    }

    // 1. Threshold Monitor
    if (cfg.trimWorkingSetOnThreshold || cfg.clearCacheOnThreshold) {
        if (m_lastThresholdCleanTick == 0 || (now - m_lastThresholdCleanTick) >= 45000ULL) {
            DetailedMemoryStats st = QueryDetailedStats();
            if (st.usedPercent >= (float)cfg.ramThresholdPercent) {
                size_t freed = 0;
                std::string status;
                if (cfg.trimWorkingSetOnThreshold && cfg.clearCacheOnThreshold) {
                    CleanAll(freed, status);
                } else if (cfg.trimWorkingSetOnThreshold) {
                    TrimProcessesWorkingSet(freed, status);
                } else if (cfg.clearCacheOnThreshold) {
                    ClearSystemCache(freed, status);
                }
                m_lastThresholdCleanTick = now;
            }
        }
    }

    // 2. Fixed Interval Monitor
    if (cfg.trimWorkingSetOnInterval || cfg.clearCacheOnInterval) {
        if (m_nextCleanTick == 0) {
            ResetIntervalTimer();
        }

        if (now >= m_nextCleanTick) {
            size_t freed = 0;
            std::string status;
            if (cfg.trimWorkingSetOnInterval && cfg.clearCacheOnInterval) {
                CleanAll(freed, status);
            } else if (cfg.trimWorkingSetOnInterval) {
                TrimProcessesWorkingSet(freed, status);
            } else if (cfg.clearCacheOnInterval) {
                ClearSystemCache(freed, status);
            }
            ResetIntervalTimer();
        }
    }
}

std::string MemoryCleaner::FormatBytes(size_t bytes) {
    char buf[64];
    double b = (double)bytes;
    if (b >= 1024.0 * 1024.0 * 1024.0) {
        snprintf(buf, sizeof(buf), "%.2f ГБ", b / (1024.0 * 1024.0 * 1024.0));
    } else if (b >= 1024.0 * 1024.0) {
        snprintf(buf, sizeof(buf), "%.1f МБ", b / (1024.0 * 1024.0));
    } else if (b >= 1024.0) {
        snprintf(buf, sizeof(buf), "%.0f КБ", b / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%zu Б", bytes);
    }
    return std::string(buf);
}

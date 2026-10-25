#pragma once

#include <string>
#include <cstddef>
#include <cstdint>

struct DetailedMemoryStats {
    double totalRAM_GB = 0.0;
    double usedRAM_GB = 0.0;
    double freeRAM_GB = 0.0;
    float usedPercent = 0.0f;
    double standbyRAM_GB = 0.0;

    double pagefileTotal_GB = 0.0;
    double pagefileUsed_GB = 0.0;
    float pagefilePercent = 0.0f;

    float peakMemoryPercent = 0.0f;
};

class MemoryCleaner {
public:
    static MemoryCleaner& Get();

    // Clean operations
    bool TrimProcessesWorkingSet(size_t& outBytesFreed, std::string& outStatus);
    bool ClearSystemCache(size_t& outBytesFreed, std::string& outStatus);
    bool PurgeStandbyList();
    bool CleanAll(size_t& outBytesFreed, std::string& outStatus);
    bool CleanRAM();
    bool CleanRAM(size_t& outBytesFreed, std::string& outStatus);

    // Background timer & threshold monitor tick
    void Update(float dt);

    // Queries
    DetailedMemoryStats QueryDetailedStats(bool forceFresh = false);
    float GetTimerProgress() const;
    int GetRemainingSeconds() const;
    void ResetIntervalTimer();
    size_t GetLastFreedBytes() const { return m_lastFreedBytes; }
    size_t GetTotalSessionFreedBytes() const { return m_totalSessionFreedBytes; }
    const std::string& GetLastActionStatus() const { return m_lastActionStatus; }
    static std::string FormatBytes(size_t bytes);

private:
    MemoryCleaner();
    ~MemoryCleaner() = default;

    bool EnablePrivilege(const wchar_t* privName);
    bool EmptyProcessWorkingSets();

    unsigned long long m_lastCleanTick = 0;
    unsigned long long m_nextCleanTick = 0;
    unsigned long long m_lastThresholdCleanTick = 0;
    int m_lastKnownIntervalMinutes = 15;
    size_t m_lastFreedBytes = 0;
    size_t m_totalSessionFreedBytes = 0;
    float m_peakUsagePercent = 0.0f;
    std::string m_lastActionStatus = "Готов к оптимизации";
};

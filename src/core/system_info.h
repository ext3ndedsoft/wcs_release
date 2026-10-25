#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

struct GPUInfo {
    std::wstring name;
    std::wstring pciDevicePath;
    bool msiSupported = false;
    bool msiEnabled = false;
    int msiPriority = 0; // 0=Default, 1=High, 2=Normal, 3=Low
    uint64_t dedicatedVRAM = 0;
};

struct SystemSpecs {
    std::string cpuName;
    int logicalCores = 0;
    int physicalCores = 0;
    
    std::vector<GPUInfo> gpus;
    std::string primaryGPUName;
    bool primaryGPUMSIEnabled = false;
    
    double totalRAM_GB = 0.0;
    double freeRAM_GB = 0.0;
    double standbyRAM_GB = 0.0;
    
    std::string osName;
    std::string osBuild;
    
    std::string activePowerPlan;
    bool isUltimatePerformanceActive = false;
    
    float currentTimerResolutionMs = 15.625f;
    float minTimerResolutionMs = 15.625f;
    float maxTimerResolutionMs = 0.500f;
};

class SystemInfo {
public:
    static SystemInfo& Get();

    void Refresh();
    void InitFast();
    void RefreshFullAsync(std::function<void()> onComplete = nullptr);

    SystemSpecs GetSpecs() const;
    bool IsInitialized() const { return m_isInitialized.load(); }
    
    // Live fast query for Timer Resolution, Memory, CPU and GPU
    float QueryLiveTimerResolution();
    void RefreshMemoryStats();
    float QueryLiveCpuUsage();
    float QueryLiveGpuUsage();

private:
    SystemInfo();
    ~SystemInfo();

    void DetectCPU();
    void DetectGPUsFast();
    void DetectGPUsDetailed();
    void DetectMemory();
    void DetectOS();
    void DetectPowerPlan();
    void DetectTimerResolution();

    void InitGpuPdh();
    void CloseGpuPdh();

    mutable std::mutex m_mutex;
    std::atomic<bool> m_isInitialized{ false };
    SystemSpecs m_specs;

    // CPU telemetry state
    uint64_t m_lastCpuQueryTick = 0;
    float m_cachedCpuUsage = 0.0f;
    uint64_t m_prevIdleTime = 0;
    uint64_t m_prevKernelTime = 0;
    uint64_t m_prevUserTime = 0;

    // GPU PDH telemetry state
    void* m_hGpuQuery = nullptr;
    void* m_hGpuCounter = nullptr;
    uint64_t m_lastGpuQueryTick = 0;
    float m_cachedGpuUsage = 0.0f;
};

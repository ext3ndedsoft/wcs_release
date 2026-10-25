#include "timer_resolution.h"
#include "debug_logger.h"
#include <windows.h>
#include <mmsystem.h>
#include <chrono>

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef NTSTATUS(NTAPI* pfnNtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
typedef NTSTATUS(NTAPI* pfnNtQueryTimerResolution)(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution);

TimerResolutionService& TimerResolutionService::Get() {
    static TimerResolutionService instance;
    return instance;
}

TimerResolutionService::TimerResolutionService() {
}

TimerResolutionService::~TimerResolutionService() {
    Stop();
}

void TimerResolutionService::Start(float targetMs) {
    if (m_running.load()) {
        m_targetMs.store(targetMs);
        return;
    }

    m_targetMs.store(targetMs);
    m_running.store(true);

    // Disable Windows Power Throttling / EcoQoS for our process so resolution isn't reverted in background
#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
    PROCESS_POWER_THROTTLING_STATE powerThrottling = { 0 };
    powerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    powerThrottling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION | PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    powerThrottling.StateMask = 0; // Disable throttling completely
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &powerThrottling, sizeof(powerThrottling));

    // Prevent system sleep / suspension while timer resolution lock is active
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

    // Call timeBeginPeriod(1) on process level immediately
    timeBeginPeriod(1);

    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::thread(&TimerResolutionService::WorkerThread, this);

    DEBUG_LOG_INFO("TIMER_RES", "Timer resolution lock 0.500ms started");
}

void TimerResolutionService::Stop() {
    if (!m_running.load()) return;

    m_running.store(false);
    if (m_worker.joinable()) {
        m_worker.join();
    }

    // Restore thread execution state
    SetThreadExecutionState(ES_CONTINUOUS);

    // Call pfnSet to cleanly revert resolution request
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        auto pfnSet = (pfnNtSetTimerResolution)GetProcAddress(hNtDll, "NtSetTimerResolution");
        if (pfnSet) {
            ULONG desiredUnits = (ULONG)(m_targetMs.load() * 10000.0f);
            if (desiredUnits < 5000) desiredUnits = 5000;
            ULONG curRes = 0;
            pfnSet(desiredUnits, FALSE, &curRes);
        }
    }

    // Restore standard multimedia period
    timeEndPeriod(1);

    DEBUG_LOG_INFO("TIMER_RES", "Timer resolution lock stopped");
}

void TimerResolutionService::WorkerThread() {
    // Thread priority ABOVE_NORMAL ensures stable scheduling without competing with game rendering
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // Call timeBeginPeriod(1) in this thread as well
    timeBeginPeriod(1);

    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) {
        timeEndPeriod(1);
        return;
    }

    auto pfnSet = (pfnNtSetTimerResolution)GetProcAddress(hNtDll, "NtSetTimerResolution");
    auto pfnQuery = (pfnNtQueryTimerResolution)GetProcAddress(hNtDll, "NtQueryTimerResolution");
    if (!pfnSet) {
        timeEndPeriod(1);
        return;
    }

    // Convert target ms to 100ns units: 0.500ms -> 5000 units (1 unit = 100ns)
    ULONG desiredUnits = (ULONG)(m_targetMs.load() * 10000.0f);
    if (desiredUnits < 5000) desiredUnits = 5000;

    ULONG curRes = 0;
    // Initial set high precision timer in NT kernel
    NTSTATUS status = pfnSet(desiredUnits, TRUE, &curRes);
    bool ok = (status >= 0);

    DebugLogger::Get().LogTimer(m_targetMs.load(), (float)curRes / 10000.0f, ok);

    // Create high-resolution waitable timer to keep quantum active on Windows 11 2004+
    HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hTimer) {
        hTimer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }

    if (hTimer) {
        LARGE_INTEGER liDueTime;
        liDueTime.QuadPart = -10000LL; // 1 ms
        SetWaitableTimer(hTimer, &liDueTime, 250, NULL, NULL, FALSE);
    }

    auto lastCheck = std::chrono::steady_clock::now();

    while (m_running.load()) {
        if (hTimer) {
            WaitForSingleObject(hTimer, 250);
        } else {
            Sleep(250);
        }

        // Every ~500ms, query the current resolution and re-assert if drifted
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheck).count() >= 500) {
            lastCheck = now;
            if (pfnQuery) {
                ULONG minRes = 0, maxRes = 0;
                curRes = 0;
                if (pfnQuery(&minRes, &maxRes, &curRes) >= 0) {
                    if (curRes > 5500) {
                        NTSTATUS reassertStatus = pfnSet(desiredUnits, TRUE, &curRes);
                        DEBUG_LOG_WARN("TIMER_RES", "Timer resolution drifted to " + 
                                       std::to_string((float)curRes / 10000.0f) + 
                                       "ms! Re-asserted 0.500ms lock (status: " + 
                                       std::to_string(reassertStatus) + ")");
                    }
                }
            }
        }
    }

    if (hTimer) {
        CancelWaitableTimer(hTimer);
        CloseHandle(hTimer);
    }

    // Balance thread-level timeBeginPeriod(1)
    timeEndPeriod(1);

    // Revert timer resolution request
    pfnSet(desiredUnits, FALSE, &curRes);
}

float TimerResolutionService::GetCurrentResolution() {
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        auto pfnQuery = (pfnNtQueryTimerResolution)GetProcAddress(hNtDll, "NtQueryTimerResolution");
        if (pfnQuery) {
            ULONG minRes = 0, maxRes = 0, curRes = 0;
            if (pfnQuery(&minRes, &maxRes, &curRes) >= 0) {
                float resMs = (float)curRes / 10000.0f;
                // If it's ~0.5ms (4900 to 5100 units), normalize to exactly 0.500 ms for UI clarity
                if (resMs >= 0.485f && resMs <= 0.515f) {
                    return 0.500f;
                }
                return resMs;
            }
        }
    }
    return 15.625f;
}

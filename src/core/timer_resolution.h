#pragma once

#include <atomic>
#include <thread>

class TimerResolutionService {
public:
    static TimerResolutionService& Get();

    void Start(float targetMs = 0.500f);
    void Stop();
    bool IsActive() const { return m_running.load(); }
    float GetCurrentResolution();

private:
    TimerResolutionService();
    ~TimerResolutionService();

    void WorkerThread();

    std::atomic<bool> m_running{ false };
    std::atomic<float> m_targetMs{ 0.500f };
    std::thread m_worker;
};

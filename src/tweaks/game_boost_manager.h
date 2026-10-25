#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include "third_party/json.hpp"

struct SupportedGameEntry {
    std::string exe;          // e.g. "cs2.exe" (lowercase)
    std::string displayName;  // e.g. "Counter-Strike 2"
};

struct TrackedGameProcess {
    DWORD pid = 0;
    std::string exe;
    std::string displayName;
    bool boostedPriority = false;
};

class GameBoostManager {
public:
    static GameBoostManager& Get();

    void Start();
    void Stop();

    nlohmann::json GetStatusJson();
    nlohmann::json ScanInstalledGames();
    bool AddCustomGame(const std::string& exeName);
    bool RemoveCustomGame(const std::string& exeName);
    bool RemoveBoostGame(const std::string& gameId, const std::string& exeName, const std::string& displayName = "");
    std::string FetchSteamCover(const std::string& gameName, const std::string& exeName, uint32_t appId = 0, const std::string& exePath = "");
    bool UpdateSettings(const nlohmann::json& params);

    bool IsGameActive() const;
    std::string GetActiveGameName() const;
    DWORD GetActiveGamePid() const;

private:
    GameBoostManager();
    ~GameBoostManager();

    void WorkerLoop();
    void CheckProcesses();
    std::string FindGameFriendlyName(const std::string& exeLower, bool& outFound, bool& outIsCustom);

    std::atomic<bool> m_running{ false };
    std::thread m_workerThread;
    std::condition_variable m_cv;
    std::mutex m_cvMutex;

    mutable std::mutex m_stateMutex;
    std::map<DWORD, TrackedGameProcess> m_activeBoostedPids; // PID -> Tracked info
    std::string m_activeGameName;
    DWORD m_activePid = 0;

    std::vector<SupportedGameEntry> m_defaultGames;
};

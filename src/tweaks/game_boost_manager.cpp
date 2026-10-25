#include "game_boost_manager.h"
#include "common_utils.h"
#include "config_manager.h"
#include "memory_cleaner.h"
#include "timer_resolution.h"
#include "native_bridge.h"
#include "debug_logger.h"
#include "resource_bundle.h"

#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <regex>

GameBoostManager& GameBoostManager::Get() {
    static GameBoostManager instance;
    return instance;
}

GameBoostManager::GameBoostManager() {
    m_defaultGames = {
        { "r5apex_dx12.exe", "Apex Legends" },
        { "r5apex.exe", "Apex Legends" },
        { "eaanticheat.gameservicelauncher.exe", "Apex Legends" },
        { "apex.exe", "Apex Legends" },
        { "marvelrivals.exe", "Marvel Rivals" },
        { "marvel-win64-shipping.exe", "Marvel Rivals" },
        { "gta5.exe", "Grand Theft Auto V" },
        { "playgtav.exe", "Grand Theft Auto V" },
        { "gta5_be.exe", "Grand Theft Auto V" },
        { "gta_sa.exe", "Arizona RP" },
        { "arizona games launcher.exe", "Arizona Games" },
        { "wallpaper64.exe", "Wallpaper Engine" },
        { "wallpaper32.exe", "Wallpaper Engine" },
        { "ride.exe", "RV There Yet" },
        { "ride-win64-shipping.exe", "RV There Yet" },
        { "cs2.exe", "Counter-Strike 2" },
        { "csgo.exe", "CS:GO" },
        { "discovery.exe", "THE FINALS" },
        { "crosshair v2.exe", "Crosshair V2" },
        { "crosshairv2.exe", "Crosshair V2" },
        { "valorant.exe", "VALORANT" },
        { "valorant-win64-shipping.exe", "VALORANT" },
        { "dota2.exe", "Dota 2" },
        { "fortniteclient-win64-shipping.exe", "Fortnite" },
        { "pubg.exe", "PUBG: BATTLEGROUNDS" },
        { "tslgame.exe", "PUBG: BATTLEGROUNDS" },
        { "rustclient.exe", "Rust" },
        { "rust.exe", "Rust" },
        { "cyberpunk2077.exe", "Cyberpunk 2077" },
        { "overwatch.exe", "Overwatch 2" },
        { "aces.exe", "War Thunder" },
        { "rainbowsix.exe", "Rainbow Six Siege" },
        { "rainbowsix_vulkan.exe", "Rainbow Six Siege" },
        { "cod.exe", "Call of Duty / Warzone" },
        { "destiny2.exe", "Destiny 2" },
        { "helldivers2.exe", "Helldivers 2" },
        { "palworld-win64-shipping.exe", "Palworld" },
        { "b1-win64-shipping.exe", "Black Myth: Wukong" },
        { "eldenring.exe", "Elden Ring" },
        { "dayz_x64.exe", "DayZ" },
        { "hl2.exe", "Team Fortress 2" },
        { "deadbydaylight-win64-shipping.exe", "Dead by Daylight" }
    };
}

GameBoostManager::~GameBoostManager() {
    Stop();
}

void GameBoostManager::Start() {
    if (m_running.load()) return;

    m_running.store(true);
    m_workerThread = std::thread(&GameBoostManager::WorkerLoop, this);
    DEBUG_LOG_INFO("GAME_BOOST", "GameBoostManager background watcher thread started");
}

void GameBoostManager::Stop() {
    if (!m_running.load()) return;

    m_running.store(false);
    m_cv.notify_all();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    DEBUG_LOG_INFO("GAME_BOOST", "GameBoostManager stopped");
}

void GameBoostManager::WorkerLoop() {
    while (m_running.load()) {
        CheckProcesses();

        std::unique_lock<std::mutex> lock(m_cvMutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(2000), [this]() {
            return !m_running.load();
        });
    }
}

void GameBoostManager::CheckProcesses() {
    const auto& cfg = ConfigManager::Get().GetConfig();
    if (!cfg.gameBoostEnabled) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_activeBoostedPids.empty()) {
            m_activeBoostedPids.clear();
            m_activeGameName = "";
            m_activePid = 0;
            NativeBridge::Get().BroadcastEvent("game_exited", { { "name", "" }, { "pid", 0 } });
        }
        return;
    }

    ScopedHandle hSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!hSnap.isValid()) return;

    std::set<DWORD> currentRunningPids;
    std::map<DWORD, std::pair<std::string, std::string>> detectedGames; // PID -> {exeLower, friendlyName}

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4) continue;
            currentRunningPids.insert(pe.th32ProcessID);

            std::wstring wsExe = pe.szExeFile;
            std::string exeLower = ToLower(WideToUtf8(wsExe));

            bool found = false;
            bool isCustom = false;
            std::string friendlyName = FindGameFriendlyName(exeLower, found, isCustom);
            if (found) {
                detectedGames[pe.th32ProcessID] = { exeLower, friendlyName };
            }
        } while (Process32NextW(hSnap, &pe));
    }
    hSnap.Close();

    // 1. Detect newly launched target game processes
    for (const auto& [pid, info] : detectedGames) {
        const std::string& exeLower = info.first;
        const std::string& friendlyName = info.second;

        bool isNew = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_activeBoostedPids.find(pid) == m_activeBoostedPids.end()) {
                isNew = true;
            }
        }

        if (isNew) {
            bool boostedPriority = false;
            if (cfg.boostHighPriority) {
                ScopedHandle hProc(OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
                if (hProc) {
                    if (SetPriorityClass(hProc, HIGH_PRIORITY_CLASS)) {
                        boostedPriority = true;
                    }
                }
            }

            if (cfg.boostStandbyPurge) {
                MemoryCleaner::Get().PurgeStandbyList();
            }

            if (cfg.boostLockTimer) {
                TimerResolutionService::Get().Start(0.500f);
            }

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                TrackedGameProcess track;
                track.pid = pid;
                track.exe = exeLower;
                track.displayName = friendlyName;
                track.boostedPriority = boostedPriority;
                m_activeBoostedPids[pid] = track;
                m_activeGameName = friendlyName;
                m_activePid = pid;
            }

            // Emit broadcast event
            NativeBridge::Get().BroadcastEvent("game_boosted", {
                { "name", friendlyName },
                { "exe", exeLower },
                { "pid", (int)pid }
            });

            // Show native toast
            std::string toastText = "🎮 Запущен " + friendlyName + "! Активирован Dynamic Boost (High Priority + Очистка Standby)";
            NativeBridge::Get().ShowToast(toastText, false);
            DEBUG_LOG_INFO("GAME_BOOST", "Dynamic Boost active for: " + friendlyName + " (PID " + std::to_string(pid) + ")");
        }
    }

    // 2. Detect closed game processes
    std::vector<DWORD> exitedPids;
    std::vector<std::string> exitedNames;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        for (const auto& [p, track] : m_activeBoostedPids) {
            if (currentRunningPids.find(p) == currentRunningPids.end()) {
                exitedPids.push_back(p);
                exitedNames.push_back(track.displayName);
            }
        }

        for (DWORD p : exitedPids) {
            m_activeBoostedPids.erase(p);
        }

        if (m_activeBoostedPids.empty()) {
            m_activeGameName = "";
            m_activePid = 0;
            if (!ConfigManager::Get().GetConfig().tweaks.timerResolution05ms) {
                TimerResolutionService::Get().Stop();
            }
        } else {
            m_activeGameName = m_activeBoostedPids.begin()->second.displayName;
            m_activePid = m_activeBoostedPids.begin()->first;
        }
    }

    for (size_t i = 0; i < exitedPids.size(); ++i) {
        DEBUG_LOG_INFO("GAME_BOOST", "Game exited: " + exitedNames[i] + " (PID " + std::to_string(exitedPids[i]) + ")");
        NativeBridge::Get().BroadcastEvent("game_exited", {
            { "name", exitedNames[i] },
            { "pid", (int)exitedPids[i] }
        });
    }
}

static std::string CleanGameTitle(const std::string& input);

std::string GameBoostManager::FindGameFriendlyName(const std::string& exeLower, bool& outFound, bool& outIsCustom) {
    outFound = false;
    outIsCustom = false;
    std::string cleanExe = ToLower(Trim(exeLower));

    const auto& cfg = ConfigManager::Get().GetConfig();

    // 1. Explicit custom boost games take highest priority
    for (const auto& cg : cfg.customBoostGames) {
        std::string cgL = ToLower(Trim(cg));
        if (cgL == cleanExe || cgL + ".exe" == cleanExe || cleanExe + ".exe" == cgL) {
            outFound = true;
            outIsCustom = true;
            std::string title = CleanGameTitle(cg);
            if (title.empty()) title = cg;
            bool cap = true;
            for (char& c : title) {
                if (isspace((unsigned char)c)) cap = true;
                else if (cap) { c = (char)toupper((unsigned char)c); cap = false; }
            }
            return title;
        }
    }

    // 2. Check if this process has been removed/ignored by user
    for (const auto& ign : cfg.ignoredBoostGames) {
        std::string ignL = ToLower(Trim(ign));
        if (!ignL.empty()) {
            if (ignL == cleanExe) return "";
            if (ignL + ".exe" == cleanExe) return "";
        }
    }

    for (const auto& g : m_defaultGames) {
        if (ToLower(g.exe) == cleanExe) {
            std::string nameL = ToLower(Trim(g.displayName));
            for (const auto& ign : cfg.ignoredBoostGames) {
                if (!ign.empty() && ToLower(Trim(ign)) == nameL) return "";
            }
            outFound = true;
            outIsCustom = false;
            return g.displayName;
        }
    }

    return "";
}

nlohmann::json GameBoostManager::GetStatusJson() {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    const auto& cfg = ConfigManager::Get().GetConfig();

    nlohmann::json res;
    res["enabled"] = cfg.gameBoostEnabled;
    res["activeGame"] = m_activeGameName;
    res["activePid"] = (int)m_activePid;

    nlohmann::json suppList = nlohmann::json::array();
    for (const auto& g : m_defaultGames) {
        suppList.push_back({
            { "exe", g.exe },
            { "name", g.displayName }
        });
    }
    res["supportedGames"] = suppList;

    nlohmann::json customList = nlohmann::json::array();
    for (const auto& cg : cfg.customBoostGames) {
        customList.push_back(cg);
    }
    res["customGames"] = customList;

    res["settings"] = {
        { "highPriority", cfg.boostHighPriority },
        { "standbyPurge", cfg.boostStandbyPurge },
        { "lockTimer", cfg.boostLockTimer }
    };

    res["success"] = true;
    return res;
}

bool GameBoostManager::AddCustomGame(const std::string& exeName) {
    std::string clean = Trim(exeName);
    if (clean.empty()) return false;
    clean = ToLower(clean);
    if (clean.find(".exe") == std::string::npos) {
        clean += ".exe";
    }

    std::string cleanBase = clean;
    if (cleanBase.size() > 4 && cleanBase.rfind(".exe") == cleanBase.size() - 4) {
        cleanBase = cleanBase.substr(0, cleanBase.size() - 4);
    }
    std::string cleanCustom = "custom_" + clean;
    std::string cleanBaseCustom = "custom_" + cleanBase;
    std::string cleanTitle = ToLower(CleanGameTitle(clean));

    auto& cfg = ConfigManager::Get().GetConfig();

    // If game was previously in ignoredBoostGames in any form (including custom_ prefix), unignore it
    bool unignored = false;
    auto prevEnd = cfg.ignoredBoostGames.end();
    auto newEnd = std::remove_if(cfg.ignoredBoostGames.begin(), cfg.ignoredBoostGames.end(),
        [&](const std::string& g) {
            std::string gl = ToLower(Trim(g));
            if (gl.empty()) return true;
            if (gl == clean || gl == cleanBase || gl + ".exe" == clean) return true;
            if (gl == cleanCustom || gl == cleanBaseCustom) return true;
            std::string stripped = gl;
            if (stripped.rfind("custom_", 0) == 0) stripped = stripped.substr(7);
            if (stripped == clean || stripped == cleanBase || stripped + ".exe" == clean) return true;
            if (!cleanTitle.empty() && (stripped == cleanTitle || ToLower(CleanGameTitle(stripped)) == cleanTitle)) return true;
            return false;
        });
    if (newEnd != prevEnd) {
        cfg.ignoredBoostGames.erase(newEnd, cfg.ignoredBoostGames.end());
        unignored = true;
    }

    for (const auto& g : cfg.customBoostGames) {
        if (ToLower(g) == clean) {
            if (unignored) ConfigManager::Get().Save();
            return true; // Already in custom games
        }
    }

    cfg.customBoostGames.push_back(clean);
    ConfigManager::Get().Save();
    DEBUG_LOG_INFO("GAME_BOOST", "Added custom boost game: " + clean);
    return true;
}

bool GameBoostManager::RemoveCustomGame(const std::string& exeName) {
    std::string clean = ToLower(Trim(exeName));
    if (clean.empty()) return false;

    auto& cfg = ConfigManager::Get().GetConfig();
    auto it = std::remove_if(cfg.customBoostGames.begin(), cfg.customBoostGames.end(),
        [&clean](const std::string& g) {
            return ToLower(g) == clean;
        });

    if (it != cfg.customBoostGames.end()) {
        cfg.customBoostGames.erase(it, cfg.customBoostGames.end());
        ConfigManager::Get().Save();
        DEBUG_LOG_INFO("GAME_BOOST", "Removed custom boost game: " + clean);
        return true;
    }
    return false;
}

bool GameBoostManager::RemoveBoostGame(const std::string& gameId, const std::string& exeName, const std::string& displayName) {
    auto& cfg = ConfigManager::Get().GetConfig();
    std::string cleanExe = ToLower(Trim(exeName));
    std::string cleanId = ToLower(Trim(gameId));
    std::string cleanName = ToLower(Trim(displayName));

    if (!cleanExe.empty()) {
        cfg.customBoostGames.erase(
            std::remove_if(cfg.customBoostGames.begin(), cfg.customBoostGames.end(),
                [&cleanExe](const std::string& s) {
                    return ToLower(Trim(s)) == cleanExe;
                }),
            cfg.customBoostGames.end()
        );
    }

    auto addToIgnored = [&cfg](const std::string& val) {
        if (val.empty()) return;
        std::string l = ToLower(Trim(val));
        if (std::find(cfg.ignoredBoostGames.begin(), cfg.ignoredBoostGames.end(), l) == cfg.ignoredBoostGames.end()) {
            cfg.ignoredBoostGames.push_back(l);
        }
    };

    if (!cleanId.empty()) addToIgnored(cleanId);
    if (!cleanExe.empty()) addToIgnored(cleanExe);
    if (!cleanName.empty()) addToIgnored(cleanName);

    ConfigManager::Get().Save();
    DEBUG_LOG_INFO("GAME_BOOST", "Removed/hidden game from Game Boost: id=" + cleanId + ", exe=" + cleanExe + ", name=" + cleanName);
    return true;
}

bool GameBoostManager::UpdateSettings(const nlohmann::json& params) {
    auto& cfg = ConfigManager::Get().GetConfig();
    bool changed = false;

    if (params.contains("enabled")) {
        cfg.gameBoostEnabled = params["enabled"].get<bool>();
        changed = true;
    }
    if (params.contains("gameBoostEnabled")) {
        cfg.gameBoostEnabled = params["gameBoostEnabled"].get<bool>();
        changed = true;
    }
    if (params.contains("highPriority")) {
        cfg.boostHighPriority = params["highPriority"].get<bool>();
        changed = true;
    }
    if (params.contains("boostHighPriority")) {
        cfg.boostHighPriority = params["boostHighPriority"].get<bool>();
        changed = true;
    }
    if (params.contains("standbyPurge")) {
        cfg.boostStandbyPurge = params["standbyPurge"].get<bool>();
        changed = true;
    }
    if (params.contains("boostStandbyPurge")) {
        cfg.boostStandbyPurge = params["boostStandbyPurge"].get<bool>();
        changed = true;
    }
    if (params.contains("lockTimer")) {
        cfg.boostLockTimer = params["lockTimer"].get<bool>();
        changed = true;
    }
    if (params.contains("boostLockTimer")) {
        cfg.boostLockTimer = params["boostLockTimer"].get<bool>();
        changed = true;
    }

    if (changed) {
        ConfigManager::Get().Save();
    }
    return true;
}

bool GameBoostManager::IsGameActive() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return !m_activeBoostedPids.empty();
}

std::string GameBoostManager::GetActiveGameName() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_activeGameName;
}

DWORD GameBoostManager::GetActiveGamePid() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_activePid;
}

#pragma comment(lib, "version.lib")

static std::string CleanGameTitle(const std::string& input) {
    std::string s = input;
    size_t dot = s.rfind(".exe");
    if (dot != std::string::npos) s = s.substr(0, dot);

    // Convert CamelCase / PascalCase to spaced words
    std::string spaced;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && isupper((unsigned char)s[i]) && (islower((unsigned char)s[i-1]) || (i + 1 < s.size() && islower((unsigned char)s[i+1])))) {
            spaced += ' ';
        }
        spaced += s[i];
    }
    s = ToLower(spaced);

    static const std::vector<std::string> suffixes = {
        "-win64-shipping", "_win64_shipping", "_dx12", "-dx12",
        "_x64", "-x64", "_release", "-release", "_be", "client", "launcher",
        "64", "32"
    };
    for (const auto& suf : suffixes) {
        if (s.size() > suf.size() && s.rfind(suf) == s.size() - suf.size()) {
            s.erase(s.size() - suf.size());
        }
    }
    for (char& c : s) {
        if (c == '_' || c == '-') c = ' ';
    }
    return Trim(s);
}

static std::string ReadExeProductNameOrDescription(const std::string& exePath) {
    if (exePath.empty()) return "";
    std::wstring pathW = Utf8ToWideStr(exePath);
    DWORD dwDummy = 0;
    DWORD dwSize = GetFileVersionInfoSizeW(pathW.c_str(), &dwDummy);
    if (dwSize == 0) return "";

    std::vector<BYTE> data(dwSize);
    if (!GetFileVersionInfoW(pathW.c_str(), 0, dwSize, data.data())) return "";

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    } *lpTranslate = nullptr;
    UINT cbTranslate = 0;

    static const std::vector<std::wstring> props = { L"ProductName", L"FileDescription" };
    static const std::set<std::string> genericTitles = {
        "bootstrappackagedgame", "unrealgame", "game", "application", "setup",
        "installer", "launcher", "client", "shipping", "unity", "engine", "crashreport"
    };

    auto toUtf8Str = [](const wchar_t* wstr) -> std::string {
        if (!wstr || !*wstr) return "";
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
        if (len <= 1) return "";
        std::string s(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &s[0], len, NULL, NULL);
        return s;
    };

    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate) && lpTranslate && cbTranslate >= sizeof(LANGANDCODEPAGE)) {
        for (unsigned int i = 0; i < (cbTranslate / sizeof(LANGANDCODEPAGE)); i++) {
            for (const auto& prop : props) {
                wchar_t subBlock[256];
                swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s", lpTranslate[i].wLanguage, lpTranslate[i].wCodePage, prop.c_str());
                wchar_t* lpBuffer = nullptr;
                UINT dwBytes = 0;
                if (VerQueryValueW(data.data(), subBlock, (LPVOID*)&lpBuffer, &dwBytes) && lpBuffer && dwBytes > 0) {
                    std::string val = Trim(toUtf8Str(lpBuffer));
                    std::string valLower = ToLower(val);
                    if (!val.empty() && !genericTitles.count(valLower) && val.length() >= 2) {
                        return val;
                    }
                }
            }
        }
    }

    // Fallback standard blocks
    static const wchar_t* fallbackBlocks[] = {
        L"\\StringFileInfo\\040904b0\\ProductName",
        L"\\StringFileInfo\\040904b0\\FileDescription",
        L"\\StringFileInfo\\000004b0\\ProductName",
        L"\\StringFileInfo\\000004b0\\FileDescription"
    };
    for (const auto* blk : fallbackBlocks) {
        wchar_t* lpBuffer = nullptr;
        UINT dwBytes = 0;
        if (VerQueryValueW(data.data(), blk, (LPVOID*)&lpBuffer, &dwBytes) && lpBuffer && dwBytes > 0) {
            std::string val = Trim(toUtf8Str(lpBuffer));
            std::string valLower = ToLower(val);
            if (!val.empty() && !genericTitles.count(valLower) && val.length() >= 2) {
                return val;
            }
        }
    }

    return "";
}

static std::string SanitizeCacheKey(const std::string& str) {
    std::string out;
    for (char c : str) {
        if (isalnum((unsigned char)c)) out += (char)tolower((unsigned char)c);
        else if (c == ' ' || c == '_' || c == '-') out += '_';
    }
    return out.empty() ? "game" : out;
}

static std::string UrlEncode(const std::string& val) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : val) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << "%20";
        } else {
            escaped << '%' << std::setw(2) << ((int)(unsigned char)c);
        }
    }
    return escaped.str();
}

static std::string HttpGet(const std::string& url) {
    std::wstring urlW = Utf8ToWideStr(url);
    HINTERNET hInternet = InternetOpenW(L"WindowsConfigStudio/3.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return "";
    
    DWORD timeout = 4000;
    InternetSetOptionW(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
    if (url.rfind("https://", 0) == 0) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hUrl = InternetOpenUrlW(hInternet, urlW.c_str(), NULL, 0, flags, 0);
    std::string result;
    if (hUrl) {
        DWORD statusCode = 0;
        DWORD codeSize = sizeof(statusCode);
        if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &codeSize, NULL)) {
            if (statusCode == 200) {
                char buf[8192];
                DWORD read = 0;
                while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0) {
                    result.append(buf, read);
                    if (result.size() > 5 * 1024 * 1024) break;
                }
            }
        }
        InternetCloseHandle(hUrl);
    }
    InternetCloseHandle(hInternet);
    return result;
}

static std::filesystem::path GetCoverCachePath(const std::string& key) {
    std::wstring resDir = ResourceBundle::GetResourcesDirectory();
    std::filesystem::path cacheDir = std::filesystem::path(resDir) / "covers" / "cache";
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    return cacheDir / (key + ".jpg");
}

static std::string DownloadAndCacheSteamCover(const std::string& gameTitle, const std::string& exeName, uint32_t knownAppId = 0, const std::string& exePath = "") {
    // 1. Try PE VersionInfo title if path provided
    std::string peTitle;
    if (!exePath.empty()) {
        peTitle = ReadExeProductNameOrDescription(exePath);
    }

    std::string clean = CleanGameTitle(gameTitle);
    if (!peTitle.empty() && peTitle != gameTitle) {
        std::string cleanPe = CleanGameTitle(peTitle);
        if (!cleanPe.empty()) clean = cleanPe;
    }
    if (clean.empty()) clean = CleanGameTitle(exeName);
    if (clean.empty()) return "";

    std::string key = SanitizeCacheKey(clean);
    std::filesystem::path cacheFile = GetCoverCachePath(key);
    std::filesystem::path noCoverMarker = cacheFile.parent_path() / (key + ".nocover");
    std::error_code ec;

    if (std::filesystem::exists(cacheFile, ec) && std::filesystem::file_size(cacheFile, ec) > 1024) {
        return "/covers/cache/" + key + ".jpg";
    }

    if (std::filesystem::exists(noCoverMarker, ec)) {
        return "";
    }

    uint32_t appId = knownAppId;
    std::string tinyImage = "";

    auto trySearchSteam = [](const std::string& term, uint32_t& outAppId, std::string& outTiny) -> bool {
        if (term.empty()) return false;
        std::string searchUrl = "https://store.steampowered.com/api/storesearch/?term=" + UrlEncode(term) + "&l=english&cc=US";
        std::string jsonResp = HttpGet(searchUrl);
        if (jsonResp.empty()) return false;
        try {
            auto j = nlohmann::json::parse(jsonResp);
            if (j.contains("items") && j["items"].is_array() && !j["items"].empty()) {
                const auto& first = j["items"][0];
                if (first.contains("id")) {
                    outAppId = first["id"].get<uint32_t>();
                }
                if (first.contains("tiny_image") && first["tiny_image"].is_string()) {
                    outTiny = first["tiny_image"].get<std::string>();
                }
                return outAppId > 0;
            }
        } catch (...) {}
        return false;
    };

    if (appId == 0) {
        if (!trySearchSteam(clean, appId, tinyImage)) {
            if (!peTitle.empty() && peTitle != clean) {
                trySearchSteam(peTitle, appId, tinyImage);
            }
            if (appId == 0 && !gameTitle.empty() && gameTitle != clean) {
                trySearchSteam(gameTitle, appId, tinyImage);
            }
            if (appId == 0 && !exeName.empty()) {
                std::string exeClean = CleanGameTitle(exeName);
                if (!exeClean.empty() && exeClean != clean) {
                    trySearchSteam(exeClean, appId, tinyImage);
                }
            }
        }
    }

    if (appId == 0) {
        std::ofstream mark(noCoverMarker);
        mark << "0";
        return "";
    }

    std::vector<std::string> candidates = {
        "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/" + std::to_string(appId) + "/library_600x900_2x.jpg",
        "https://cdn.cloudflare.steamstatic.com/steam/apps/" + std::to_string(appId) + "/library_600x900_2x.jpg",
        "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/" + std::to_string(appId) + "/library_600x900.jpg",
        "https://cdn.cloudflare.steamstatic.com/steam/apps/" + std::to_string(appId) + "/library_600x900.jpg",
        "https://shared.cloudflare.steamstatic.com/store_item_assets/steam/apps/" + std::to_string(appId) + "/header.jpg",
        "https://cdn.cloudflare.steamstatic.com/steam/apps/" + std::to_string(appId) + "/header.jpg",
        "https://steamcdn-a.akamaihd.net/steam/apps/" + std::to_string(appId) + "/library_600x900_2x.jpg",
        "https://steamcdn-a.akamaihd.net/steam/apps/" + std::to_string(appId) + "/header.jpg"
    };

    if (!tinyImage.empty()) {
        std::string tUrl = tinyImage;
        size_t qPos = tUrl.find('?');
        if (qPos != std::string::npos) tUrl = tUrl.substr(0, qPos);
        size_t fPos = tUrl.rfind('/');
        if (fPos != std::string::npos) {
            std::string prefix = tUrl.substr(0, fPos + 1);
            candidates.push_back(prefix + "library_600x900.jpg");
            candidates.push_back(prefix + "header.jpg");
            candidates.push_back(prefix + "capsule_616x353.jpg");
        }
        candidates.push_back(tinyImage);
    }

    for (const auto& url : candidates) {
        std::string imgData = HttpGet(url);
        if (imgData.size() > 1024) {
            std::ofstream out(cacheFile, std::ios::binary);
            if (out.is_open()) {
                out.write(imgData.data(), imgData.size());
                out.close();
                DEBUG_LOG_INFO("GAME_BOOST", "Downloaded Steam cover for " + clean + " (AppID " + std::to_string(appId) + ") to " + cacheFile.string());
                return "/covers/cache/" + key + ".jpg";
            }
        }
    }

    std::string appDetailsUrl = "https://store.steampowered.com/api/appdetails?appids=" + std::to_string(appId);
    std::string dtResp = HttpGet(appDetailsUrl);
    if (!dtResp.empty()) {
        try {
            auto j = nlohmann::json::parse(dtResp);
            std::string idStr = std::to_string(appId);
            if (j.contains(idStr) && j[idStr].contains("data")) {
                const auto& d = j[idStr]["data"];
                if (d.contains("header_image") && d["header_image"].is_string()) {
                    std::string hUrl = d["header_image"].get<std::string>();
                    std::string imgData = HttpGet(hUrl);
                    if (imgData.size() > 1024) {
                        std::ofstream out(cacheFile, std::ios::binary);
                        if (out.is_open()) {
                            out.write(imgData.data(), imgData.size());
                            out.close();
                            return "/covers/cache/" + key + ".jpg";
                        }
                    }
                }
            }
        } catch (...) {}
    }

    std::ofstream mark(noCoverMarker);
    mark << std::to_string(appId);
    return "";
}

static std::string GetCoverForGameLocalOnly(const std::string& nameOrFolder, const std::string& displayName = "", const std::string& exeName = "") {
    std::string s = ToLower(nameOrFolder + " " + displayName + " " + exeName);

    // Exact matches and specific games first
    if (s.find("gta_sa") != std::string::npos || s.find("san andreas") != std::string::npos || s.find("arizona") != std::string::npos) return "/covers/gta_sa.jpg";
    if (s.find("wallpaper") != std::string::npos) return "/covers/wallpaper_engine.jpg";
    if (s.find("rv there yet") != std::string::npos || s.find("rv_there_yet") != std::string::npos || s.find("ride") != std::string::npos) return "/covers/rv_there_yet.jpg";
    if (s.find("apex") != std::string::npos) return "/covers/apex.jpg";
    if (s.find("cs2") != std::string::npos || s.find("counter-strike") != std::string::npos || s.find("csgo") != std::string::npos) return "/covers/cs2.jpg";
    if (s.find("gta") != std::string::npos || s.find("grand theft auto") != std::string::npos) return "/covers/gta5.jpg";
    if (s.find("marvel") != std::string::npos) return "/covers/marvelrivals.jpg";
    if (s.find("finals") != std::string::npos || s.find("discovery") != std::string::npos) return "/covers/thefinals.jpg";
    if (s.find("crosshair") != std::string::npos) return "/covers/crosshairv2.jpg";
    if (s.find("valorant") != std::string::npos) return "/covers/valorant.jpg";
    if (s.find("dota") != std::string::npos) return "/covers/dota2.jpg";
    if (s.find("fortnite") != std::string::npos) return "/covers/fortnite.jpg";
    if (s.find("pubg") != std::string::npos || s.find("battlegrounds") != std::string::npos) return "/covers/pubg.jpg";
    if (s.find("rust") != std::string::npos) return "/covers/rust.jpg";
    if (s.find("cyberpunk") != std::string::npos) return "/covers/cyberpunk.jpg";
    if (s.find("overwatch") != std::string::npos) return "/covers/overwatch2.jpg";
    if (s.find("warthunder") != std::string::npos || s.find("war thunder") != std::string::npos) return "/covers/warthunder.jpg";
    if (s.find("rainbow") != std::string::npos || s.find("r6") != std::string::npos) return "/covers/rainbowsix.jpg";
    if (s.find("cod") != std::string::npos || s.find("call of duty") != std::string::npos || s.find("warzone") != std::string::npos) return "/covers/cod.jpg";
    if (s.find("destiny") != std::string::npos) return "/covers/destiny2.jpg";
    if (s.find("helldivers") != std::string::npos) return "/covers/helldivers2.jpg";
    if (s.find("palworld") != std::string::npos) return "/covers/palworld.jpg";
    if (s.find("tf2") != std::string::npos || s.find("team fortress") != std::string::npos) return "/covers/tf2.jpg";
    if (s.find("dead by daylight") != std::string::npos || s.find("deadbydaylight") != std::string::npos) return "/covers/deadbydaylight.jpg";
    if (s.find("wukong") != std::string::npos || s.find("black myth") != std::string::npos) return "/covers/wukong.jpg";
    if (s.find("elden") != std::string::npos) return "/covers/eldenring.jpg";
    if (s.find("dayz") != std::string::npos) return "/covers/dayz.jpg";

    // Check disk cache
    std::string key = SanitizeCacheKey(CleanGameTitle(displayName.empty() ? nameOrFolder : displayName));
    std::filesystem::path cacheFile = GetCoverCachePath(key);
    std::error_code ec;
    if (std::filesystem::exists(cacheFile, ec) && std::filesystem::file_size(cacheFile, ec) > 1024) {
        return "/covers/cache/" + key + ".jpg";
    }

    return "/covers/default_game.jpg";
}

std::string GameBoostManager::FetchSteamCover(const std::string& gameName, const std::string& exeName, uint32_t appId, const std::string& exePath) {
    std::string local = GetCoverForGameLocalOnly(gameName, gameName, exeName);
    if (local != "/covers/default_game.jpg") return local;

    return DownloadAndCacheSteamCover(gameName, exeName, appId, exePath);
}

static bool IsIgnoredExe(const std::string& exeNameLower) {
    static const std::vector<std::string> kBadSubstrings = {
        "unins", "setup", "installer", "update", "patch", "crash", "reporter",
        "redist", "vcredist", "dxwebsetup", "directx", "elevate", "cef_",
        "unitycrash", "easyanticheat_setup", "epicgameslauncher", "steamservice",
        "steamerrorreporter", "subinacl", "dotnet", "reset", "clean", "diagnostic",
        "browser", "node", "electron", "helper", "backend", "majestic", "ragemp"
    };
    for (const auto& bad : kBadSubstrings) {
        if (exeNameLower.find(bad) != std::string::npos) return true;
    }
    return false;
}

nlohmann::json GameBoostManager::ScanInstalledGames() {
    struct KnownGameEntry {
        std::string id;
        std::string name;
        std::vector<std::string> exes;
        std::vector<std::string> steamFolders;
        std::vector<std::string> commonPaths;
        std::string cover;
    };

    static const std::vector<KnownGameEntry> kKnownGames = {
        { "apex", "Apex Legends", { "r5apex_dx12.exe", "r5apex.exe", "eaanticheat.gameservicelauncher.exe", "apex.exe" }, { "Apex Legends" }, { "D:\\SteamLibrary\\steamapps\\common\\Apex Legends", "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Apex Legends", "C:\\SteamLibrary\\steamapps\\common\\Apex Legends" }, "/covers/apex.jpg" },
        { "gta5", "Grand Theft Auto V", { "gta5.exe", "playgtav.exe", "gta5_be.exe" }, { "Grand Theft Auto V" }, { "D:\\GTAV", "C:\\Games\\GTA V", "D:\\Games\\GTA V", "C:\\Program Files\\Epic Games\\GTAV" }, "/covers/gta5.jpg" },
        { "arizona", "Arizona RP", { "gta_sa.exe", "arizona games launcher.exe" }, {}, { "D:\\Arizona\\Arizona Games Launcher\\bin\\arizona", "D:\\Arizona\\Arizona Games Launcher", "D:\\Arizona", "C:\\Arizona Games" }, "/covers/gta_sa.jpg" },
        { "wallpaper_engine", "Wallpaper Engine", { "wallpaper64.exe", "wallpaper32.exe" }, { "wallpaper_engine" }, { "D:\\SteamLibrary\\steamapps\\common\\wallpaper_engine", "C:\\Program Files (x86)\\Steam\\steamapps\\common\\wallpaper_engine" }, "/covers/wallpaper_engine.jpg" },
        { "rv_there_yet", "RV There Yet", { "ride.exe", "ride-win64-shipping.exe" }, {}, { "C:\\Games\\RV There Yet", "D:\\Games\\RV There Yet" }, "/covers/rv_there_yet.jpg" },
        { "marvelrivals", "Marvel Rivals", { "marvelrivals.exe", "marvel-win64-shipping.exe" }, { "MarvelRivals" }, { "D:\\SteamLibrary\\steamapps\\common\\MarvelRivals" }, "/covers/marvelrivals.jpg" },
        { "cs2", "Counter-Strike 2", { "cs2.exe", "csgo.exe" }, { "Counter-Strike Global Offensive" }, {}, "/covers/cs2.jpg" },
        { "thefinals", "THE FINALS", { "discovery.exe" }, { "The Finals" }, {}, "/covers/thefinals.jpg" },
        { "crosshairv2", "Crosshair V2", { "crosshair v2.exe", "crosshairv2.exe" }, { "Crosshair V2" }, {}, "/covers/crosshairv2.jpg" },
        { "valorant", "VALORANT", { "valorant.exe", "valorant-win64-shipping.exe" }, {}, { "C:\\Riot Games\\VALORANT", "D:\\Riot Games\\VALORANT", "C:\\Riot Games\\Riot Client" }, "/covers/valorant.jpg" },
        { "dota2", "Dota 2", { "dota2.exe" }, { "dota 2 beta" }, {}, "/covers/dota2.jpg" },
        { "fortnite", "Fortnite", { "fortniteclient-win64-shipping.exe", "fortnitelauncher.exe" }, {}, { "C:\\Program Files\\Epic Games\\Fortnite", "D:\\Epic Games\\Fortnite" }, "/covers/fortnite.jpg" },
        { "pubg", "PUBG: BATTLEGROUNDS", { "tslgame.exe", "pubg.exe" }, { "PUBG" }, {}, "/covers/pubg.jpg" },
        { "rust", "Rust", { "rustclient.exe", "rust.exe" }, { "Rust" }, {}, "/covers/rust.jpg" },
        { "cyberpunk", "Cyberpunk 2077", { "cyberpunk2077.exe" }, { "Cyberpunk 2077" }, { "C:\\Games\\Cyberpunk 2077", "D:\\Games\\Cyberpunk 2077" }, "/covers/cyberpunk.jpg" },
        { "overwatch2", "Overwatch 2", { "overwatch.exe" }, { "Overwatch" }, { "C:\\Program Files (x86)\\Overwatch", "D:\\Overwatch" }, "/covers/overwatch2.jpg" },
        { "warthunder", "War Thunder", { "aces.exe", "launcher.exe" }, { "War Thunder" }, { "C:\\Games\\WarThunder", "D:\\Games\\WarThunder" }, "/covers/warthunder.jpg" },
        { "rainbowsix", "Tom Clancy's Rainbow Six Siege", { "rainbowsix.exe", "rainbowsix_vulkan.exe" }, { "Rainbow Six Siege" }, {}, "/covers/rainbowsix.jpg" },
        { "cod", "Call of Duty / Warzone", { "cod.exe", "bootstrapper.exe" }, { "Call of Duty HQ" }, { "C:\\Program Files (x86)\\Call of Duty", "D:\\Call of Duty" }, "/covers/cod.jpg" },
        { "destiny2", "Destiny 2", { "destiny2.exe" }, { "Destiny 2" }, {}, "/covers/destiny2.jpg" },
        { "helldivers2", "Helldivers 2", { "helldivers2.exe" }, { "HELLDIVERS 2" }, {}, "/covers/helldivers2.jpg" },
        { "palworld", "Palworld", { "palworld-win64-shipping.exe" }, { "Palworld" }, {}, "/covers/palworld.jpg" },
        { "tf2", "Team Fortress 2", { "hl2.exe" }, { "Team Fortress 2" }, {}, "/covers/tf2.jpg" },
        { "deadbydaylight", "Dead by Daylight", { "deadbydaylight-win64-shipping.exe" }, { "Dead by Daylight" }, {}, "/covers/deadbydaylight.jpg" },
        { "wukong", "Black Myth: Wukong", { "b1-win64-shipping.exe" }, { "Black Myth Wukong" }, {}, "/covers/wukong.jpg" },
        { "eldenring", "Elden Ring", { "eldenring.exe" }, { "ELDEN RING" }, {}, "/covers/eldenring.jpg" },
        { "dayz", "DayZ", { "dayz_x64.exe" }, { "DayZ" }, {}, "/covers/dayz.jpg" }
    };

    std::error_code ec;

    // 1. Discover Steam library folders
    std::vector<std::filesystem::path> steamLibraries;
    std::vector<std::string> driveCandidates = { "C", "D", "E", "F", "G", "H" };
    for (const auto& d : driveCandidates) {
        std::filesystem::path p1 = d + ":\\SteamLibrary";
        if (std::filesystem::exists(p1, ec)) steamLibraries.push_back(p1);
        std::filesystem::path p2 = d + ":\\Program Files (x86)\\Steam";
        if (std::filesystem::exists(p2, ec)) steamLibraries.push_back(p2);
        std::filesystem::path p3 = d + ":\\Games\\SteamLibrary";
        if (std::filesystem::exists(p3, ec)) steamLibraries.push_back(p3);
    }

    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char val[MAX_PATH] = { 0 };
        DWORD valSize = sizeof(val);
        if (RegQueryValueExA(hKey, "SteamPath", NULL, NULL, (LPBYTE)val, &valSize) == ERROR_SUCCESS) {
            std::filesystem::path sp(val);
            if (std::filesystem::exists(sp, ec)) steamLibraries.push_back(sp);
        }
        RegCloseKey(hKey);
    }

    std::vector<std::filesystem::path> extraLibs;
    for (const auto& lib : steamLibraries) {
        std::filesystem::path vdf = lib / "steamapps" / "libraryfolders.vdf";
        if (std::filesystem::exists(vdf, ec)) {
            std::ifstream f(vdf);
            std::string line;
            while (std::getline(f, line)) {
                size_t pPos = line.find("\"path\"");
                if (pPos != std::string::npos) {
                    size_t firstQuote = line.find('"', pPos + 6);
                    if (firstQuote != std::string::npos) {
                        size_t secondQuote = line.find('"', firstQuote + 1);
                        if (secondQuote != std::string::npos) {
                            std::string parsed = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                            std::string norm;
                            for (size_t i = 0; i < parsed.size(); ++i) {
                                if (parsed[i] == '\\' && i + 1 < parsed.size() && parsed[i+1] == '\\') {
                                    norm += '\\';
                                    ++i;
                                } else {
                                    norm += parsed[i];
                                }
                            }
                            std::filesystem::path p(norm);
                            if (std::filesystem::exists(p, ec)) {
                                extraLibs.push_back(p);
                            }
                        }
                    }
                }
            }
        }
    }
    steamLibraries.insert(steamLibraries.end(), extraLibs.begin(), extraLibs.end());
    std::sort(steamLibraries.begin(), steamLibraries.end());
    steamLibraries.erase(std::unique(steamLibraries.begin(), steamLibraries.end()), steamLibraries.end());

    // 2. Discover running processes
    std::map<std::string, DWORD> runningExes;
    {
        ScopedHandle hSnap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (hSnap.isValid()) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (pe.th32ProcessID <= 4) continue;
                    std::string el = ToLower(WideToUtf8(pe.szExeFile));
                    runningExes[el] = pe.th32ProcessID;
                } while (Process32NextW(hSnap, &pe));
            }
        }
    }

    // 2.1 Parse Steam appmanifest_*.acf files across all discovered library folders
    struct SteamAppManifest {
        uint32_t appId = 0;
        std::string name;
        std::string installdir;
    };
    std::map<std::string, SteamAppManifest> steamAppManifests;
    for (const auto& lib : steamLibraries) {
        std::filesystem::path appsDir = lib / "steamapps";
        if (!std::filesystem::exists(appsDir, ec)) continue;
        try {
            for (auto it = std::filesystem::directory_iterator(appsDir, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                try {
                    if (!it->is_regular_file(ec)) continue;
                    std::string fn = it->path().filename().string();
                    if (fn.rfind("appmanifest_", 0) == 0 && fn.ends_with(".acf")) {
                        std::ifstream mf(it->path());
                        std::string mline;
                        SteamAppManifest sam;
                        while (std::getline(mf, mline)) {
                            auto getQuotedVal = [](const std::string& line, const std::string& key) -> std::string {
                                size_t p = line.find("\"" + key + "\"");
                                if (p == std::string::npos) return "";
                                size_t q1 = line.find('"', p + key.length() + 2);
                                if (q1 == std::string::npos) return "";
                                size_t q2 = line.find('"', q1 + 1);
                                if (q2 == std::string::npos) return "";
                                return line.substr(q1 + 1, q2 - q1 - 1);
                            };
                            std::string appidStr = getQuotedVal(mline, "appid");
                            if (!appidStr.empty()) {
                                try { sam.appId = (uint32_t)std::stoul(appidStr); } catch (...) {}
                            }
                            std::string nm = getQuotedVal(mline, "name");
                            if (!nm.empty()) sam.name = nm;
                            std::string idir = getQuotedVal(mline, "installdir");
                            if (!idir.empty()) sam.installdir = idir;
                        }
                        if (sam.appId > 0 && !sam.installdir.empty()) {
                            steamAppManifests[ToLower(sam.installdir)] = sam;
                        }
                    }
                } catch (...) {}
            }
        } catch (...) {}
    }

    auto& cfg = ConfigManager::Get().GetConfig();
    auto isGameIgnored = [&cfg](const std::string& id, const std::string& exe, const std::string& name) {
        std::string idL = ToLower(Trim(id));
        std::string exeL = ToLower(Trim(exe));
        std::string nameL = ToLower(Trim(name));
        // Explicit custom games added by user are NEVER ignored
        for (const auto& cg : cfg.customBoostGames) {
            std::string cgL = ToLower(Trim(cg));
            if (!cgL.empty() && (exeL == cgL || exeL + ".exe" == cgL || cgL + ".exe" == exeL)) {
                return false;
            }
        }
        for (const auto& ign : cfg.ignoredBoostGames) {
            std::string ignL = ToLower(Trim(ign));
            if (ignL.empty()) continue;
            if (idL == ignL || exeL == ignL || nameL == ignL) return true;
            if (exeL == ignL + ".exe" || ignL == exeL + ".exe") return true;
        }
        return false;
    };

    // 3. Scan each known game
    nlohmann::json result = nlohmann::json::array();
    std::set<std::string> addedIds;
    std::set<std::string> addedPaths;
    std::set<std::string> addedExes;

    for (const auto& game : kKnownGames) {
        bool installed = false;
        std::string foundPath = "";
        std::string foundExe = game.exes.empty() ? "" : game.exes[0];

        bool isRunning = false;
        DWORD runningPid = 0;
        for (const auto& exe : game.exes) {
            std::string el = ToLower(exe);
            auto it = runningExes.find(el);
            if (it != runningExes.end()) {
                isRunning = true;
                runningPid = it->second;
                foundExe = exe;
                installed = true;
                break;
            }
        }

        // Check Steam library directories
        if (!installed) {
            for (const auto& lib : steamLibraries) {
                for (const auto& sf : game.steamFolders) {
                    std::filesystem::path gameDir = lib / "steamapps" / "common" / sf;
                    if (std::filesystem::exists(gameDir, ec)) {
                        for (const auto& exe : game.exes) {
                            std::filesystem::path ep = gameDir / exe;
                            if (std::filesystem::exists(ep, ec)) {
                                installed = true;
                                foundExe = exe;
                                foundPath = ep.string();
                                break;
                            }
                        }
                        if (!installed) {
                            try {
                                for (const auto& sub : std::filesystem::directory_iterator(gameDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
                                    if (ec) break;
                                    if (sub.is_directory(ec)) {
                                        for (const auto& exe : game.exes) {
                                            std::filesystem::path ep = sub.path() / exe;
                                            if (std::filesystem::exists(ep, ec)) {
                                                installed = true;
                                                foundExe = exe;
                                                foundPath = ep.string();
                                                break;
                                            }
                                        }
                                        if (installed) break;
                                    }
                                }
                            } catch (...) {}
                        }
                        if (installed) break;
                    }
                }
                if (installed) break;
            }
        }

        // Check common and custom paths
        if (!installed) {
            for (const auto& cp : game.commonPaths) {
                std::filesystem::path cpPath(cp);
                if (std::filesystem::exists(cpPath, ec)) {
                    if (std::filesystem::is_directory(cpPath, ec)) {
                        for (const auto& exe : game.exes) {
                            std::filesystem::path ep = cpPath / exe;
                            if (std::filesystem::exists(ep, ec)) {
                                installed = true;
                                foundExe = exe;
                                foundPath = ep.string();
                                break;
                            }
                        }
                        if (!installed) {
                            try {
                                for (const auto& sub : std::filesystem::directory_iterator(cpPath, std::filesystem::directory_options::skip_permission_denied, ec)) {
                                    if (ec) break;
                                    if (sub.is_directory(ec)) {
                                        for (const auto& exe : game.exes) {
                                            std::filesystem::path ep = sub.path() / exe;
                                            if (std::filesystem::exists(ep, ec)) {
                                                installed = true;
                                                foundExe = exe;
                                                foundPath = ep.string();
                                                break;
                                            }
                                        }
                                        if (installed) break;
                                        try {
                                            for (const auto& sub2 : std::filesystem::directory_iterator(sub.path(), std::filesystem::directory_options::skip_permission_denied, ec)) {
                                                if (ec) break;
                                                if (sub2.is_directory(ec)) {
                                                    for (const auto& exe : game.exes) {
                                                        std::filesystem::path ep = sub2.path() / exe;
                                                        if (std::filesystem::exists(ep, ec)) {
                                                            installed = true;
                                                            foundExe = exe;
                                                            foundPath = ep.string();
                                                            break;
                                                        }
                                                    }
                                                    if (installed) break;
                                                }
                                            }
                                        } catch (...) {}
                                        if (installed) break;
                                    }
                                }
                            } catch (...) {}
                        }
                    } else {
                        installed = true;
                        foundPath = cp;
                        foundExe = cpPath.filename().string();
                    }
                    if (installed) break;
                }
            }
        }

        if (installed) {
            std::string exeLower = ToLower(foundExe);
            if (!exeLower.empty() && addedExes.count(exeLower)) continue;
            if (isGameIgnored(game.id, foundExe, game.name)) continue;

            if (foundPath.empty() && isRunning && runningPid > 0) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, runningPid);
                if (hProc) {
                    wchar_t pBuf[MAX_PATH] = { 0 };
                    DWORD pLen = MAX_PATH;
                    if (QueryFullProcessImageNameW(hProc, 0, pBuf, &pLen)) {
                        foundPath = WideToUtf8(pBuf);
                    }
                    CloseHandle(hProc);
                }
            }

            addedIds.insert(ToLower(game.id));
            if (!exeLower.empty()) addedExes.insert(exeLower);
            if (!foundPath.empty()) {
                addedPaths.insert(ToLower(foundPath));
                std::filesystem::path fp(foundPath);
                addedPaths.insert(ToLower(fp.parent_path().string()));
            }

            nlohmann::json item;
            item["id"] = game.id;
            item["name"] = game.name;
            item["exe"] = foundExe;
            item["path"] = foundPath;
            item["cover"] = game.cover;
            item["appId"] = 0;
            item["running"] = isRunning;
            item["pid"] = (int)runningPid;
            item["boosted"] = isRunning;
            result.push_back(item);
        }
    }

    // 4. Generic auto-discovery for all games in Steam libraries common folder
    for (const auto& lib : steamLibraries) {
        std::filesystem::path commonDir = lib / "steamapps" / "common";
        if (!std::filesystem::exists(commonDir, ec)) continue;

        try {
            for (auto it = std::filesystem::directory_iterator(commonDir, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                try {
                    if (!it->is_directory(ec)) continue;
                    std::string folderName = it->path().filename().string();
                    std::string folderLower = ToLower(folderName);
                    std::string pathLower = ToLower(it->path().string());

                    if (folderLower == "steamworks shared" || folderLower == "steamvr" || folderLower == "_commonredist") {
                        continue;
                    }
                    if (folderLower.find("majestic") != std::string::npos || folderLower.find("ragemp") != std::string::npos) {
                        continue;
                    }
                    if (addedPaths.count(pathLower)) continue;

                    bool alreadyAdded = false;
                    for (const auto& id : addedIds) {
                        if (folderLower.find(id) != std::string::npos || id.find(folderLower) != std::string::npos) {
                            alreadyAdded = true;
                            break;
                        }
                    }
                    if (alreadyAdded) continue;

                    std::string bestExe = "";
                    std::string bestPath = "";
                    for (auto fIt = std::filesystem::directory_iterator(it->path(), std::filesystem::directory_options::skip_permission_denied, ec);
                         !ec && fIt != std::filesystem::directory_iterator(); fIt.increment(ec)) {
                        if (ec) break;
                        try {
                            if (fIt->is_regular_file(ec) && fIt->path().extension() == ".exe") {
                                std::string fn = fIt->path().filename().string();
                                if (!IsIgnoredExe(ToLower(fn))) {
                                    bestExe = fn;
                                    bestPath = fIt->path().string();
                                    break;
                                }
                            }
                        } catch (...) {}
                    }

                    if (!bestExe.empty() && addedPaths.find(ToLower(bestPath)) == addedPaths.end()) {
                        std::string exeLower = ToLower(bestExe);
                        if (!exeLower.empty() && addedExes.count(exeLower)) continue;

                        std::string gameId = "steam_" + folderLower;
                        std::string realName = folderName;
                        uint32_t steamAppId = 0;
                        if (steamAppManifests.count(folderLower)) {
                            steamAppId = steamAppManifests[folderLower].appId;
                            if (!steamAppManifests[folderLower].name.empty()) {
                                realName = steamAppManifests[folderLower].name;
                            }
                        }

                        if (isGameIgnored(gameId, bestExe, realName)) continue;

                        addedIds.insert(gameId);
                        if (!exeLower.empty()) addedExes.insert(exeLower);
                        addedPaths.insert(ToLower(bestPath));
                        addedPaths.insert(pathLower);

                        bool isRunning = (runningExes.find(ToLower(bestExe)) != runningExes.end());
                        DWORD pid = isRunning ? runningExes[ToLower(bestExe)] : 0;

                        nlohmann::json item;
                        item["id"] = gameId;
                        item["name"] = realName;
                        item["exe"] = bestExe;
                        item["path"] = bestPath;
                        item["cover"] = GetCoverForGameLocalOnly(folderName, realName, bestExe);
                        item["appId"] = (int)steamAppId;
                        item["running"] = isRunning;
                        item["pid"] = (int)pid;
                        item["boosted"] = isRunning;
                        result.push_back(item);
                    }
                } catch (...) {}
            }
        } catch (...) {}
    }

    // 5. Generic auto-discovery for common game directories (C:\Games, D:\Games, etc.)
    std::vector<std::string> gameRoots = { "C:\\Games", "D:\\Games", "D:\\" };
    static const std::set<std::string> kSkipRootFolders = {
        "windows", "program files", "program files (x86)", "programdata", "users",
        "appdata", "recovery", "$recycle.bin", "system volume information", "backup",
        "temp", "varriances", "vortex mods", "wondershare", "откаты", "steamlibrary"
    };

    for (const auto& gr : gameRoots) {
        try {
            std::filesystem::path grPath(gr);
            if (!std::filesystem::exists(grPath, ec)) continue;

            for (auto it = std::filesystem::directory_iterator(grPath, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                try {
                    if (!it->is_directory(ec)) continue;
                    std::string folderName = it->path().filename().string();
                    std::string folderLower = ToLower(folderName);
                    std::string pathLower = ToLower(it->path().string());
                    if (kSkipRootFolders.count(folderLower)) continue;
                    if (folderLower.find("majestic") != std::string::npos || folderLower.find("ragemp") != std::string::npos) continue;
                    if (addedPaths.count(pathLower)) continue;

                    bool alreadyAdded = false;
                    for (const auto& id : addedIds) {
                        if (folderLower.find(id) != std::string::npos || id.find(folderLower) != std::string::npos) {
                            alreadyAdded = true;
                            break;
                        }
                    }
                    if (alreadyAdded) continue;

                    std::string bestExe = "";
                    std::string bestPath = "";
                    for (auto fIt = std::filesystem::directory_iterator(it->path(), std::filesystem::directory_options::skip_permission_denied, ec);
                         !ec && fIt != std::filesystem::directory_iterator(); fIt.increment(ec)) {
                        if (ec) break;
                        try {
                            if (fIt->is_regular_file(ec) && fIt->path().extension() == ".exe") {
                                std::string fn = fIt->path().filename().string();
                                if (!IsIgnoredExe(ToLower(fn))) {
                                    bestExe = fn;
                                    bestPath = fIt->path().string();
                                    break;
                                }
                            }
                        } catch (...) {}
                    }

                    if (!bestExe.empty() && addedPaths.find(ToLower(bestPath)) == addedPaths.end()) {
                        std::string exeLower = ToLower(bestExe);
                        if (!exeLower.empty() && addedExes.count(exeLower)) continue;

                        std::string gameId = "game_" + folderLower;
                        if (isGameIgnored(gameId, bestExe, folderName)) continue;

                        addedIds.insert(gameId);
                        if (!exeLower.empty()) addedExes.insert(exeLower);
                        addedPaths.insert(ToLower(bestPath));
                        addedPaths.insert(pathLower);

                        bool isRunning = (runningExes.find(ToLower(bestExe)) != runningExes.end());
                        DWORD pid = isRunning ? runningExes[ToLower(bestExe)] : 0;

                        nlohmann::json item;
                        item["id"] = gameId;
                        item["name"] = folderName;
                        item["exe"] = bestExe;
                        item["path"] = bestPath;
                        item["cover"] = GetCoverForGameLocalOnly(folderName, folderName, bestExe);
                        item["appId"] = 0;
                        item["running"] = isRunning;
                        item["pid"] = (int)pid;
                        item["boosted"] = isRunning;
                        result.push_back(item);
                    }
                } catch (...) {}
            }
        } catch (...) {}
    }

    // 6. Custom boost games from configuration
    // Clean up any legacy majestic/ragemp mentions if present
    cfg.customBoostGames.erase(
        std::remove_if(cfg.customBoostGames.begin(), cfg.customBoostGames.end(),
            [](const std::string& g) {
                std::string l = ToLower(g);
                return l.find("majestic") != std::string::npos || l.find("ragemp") != std::string::npos;
            }),
        cfg.customBoostGames.end()
    );

    for (const auto& cg : cfg.customBoostGames) {
        std::string cgLower = ToLower(Trim(cg));
        if (cgLower.empty()) continue;
        std::string gameId = "custom_" + cgLower;
        if (addedIds.count(gameId)) continue;
        if (addedExes.count(cgLower)) continue;

        // Custom boost games were explicitly added by user. They must ALWAYS appear!
        // Clean up any lingering ignored entries for this custom game
        std::string cleanBase = cgLower;
        if (cleanBase.size() > 4 && cleanBase.rfind(".exe") == cleanBase.size() - 4) {
            cleanBase = cleanBase.substr(0, cleanBase.size() - 4);
        }
        cfg.ignoredBoostGames.erase(
            std::remove_if(cfg.ignoredBoostGames.begin(), cfg.ignoredBoostGames.end(),
                [&](const std::string& ig) {
                    std::string igL = ToLower(Trim(ig));
                    if (igL == cgLower || igL == gameId || igL == cleanBase || igL == "custom_" + cleanBase) return true;
                    if (igL.rfind("custom_", 0) == 0 && igL.substr(7) == cgLower) return true;
                    return false;
                }),
            cfg.ignoredBoostGames.end()
        );

        bool isRunning = (runningExes.find(cgLower) != runningExes.end());
        DWORD pid = isRunning ? runningExes[cgLower] : 0;
        
        addedIds.insert(gameId);
        addedExes.insert(cgLower);

        std::string rawTitle = CleanGameTitle(cg);
        if (rawTitle.empty()) rawTitle = cg;
        std::string displayName = rawTitle;
        bool capNext = true;
        for (char& c : displayName) {
            if (isspace((unsigned char)c)) {
                capNext = true;
            } else if (capNext) {
                c = (char)toupper((unsigned char)c);
                capNext = false;
            }
        }

        nlohmann::json item;
        item["id"] = gameId;
        item["name"] = displayName;
        item["exe"] = cg;
        item["path"] = "";
        item["cover"] = GetCoverForGameLocalOnly(displayName, rawTitle, cg);
        item["appId"] = 0;
        item["running"] = isRunning;
        item["pid"] = (int)pid;
        item["boosted"] = isRunning;
        result.push_back(item);
    }

    return result;
}


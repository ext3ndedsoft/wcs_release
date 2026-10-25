#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <functional>
#include "app_version.h"
#include "third_party/json.hpp"

struct UpdateInfo {
    bool available = false;
    std::string currentVersion = WCS_DISPLAY_VERSION;
    std::string latestVersion = "";
    std::string downloadUrl = "";
    std::string releaseUrl = "";
    std::string changelog = "";
    std::string publishDate = "";
    std::string channel = "";
    std::string errorMsg = "";
};

class UpdateManager {
public:
    static UpdateManager& Get();

    void Init();
    void CheckUpdatesAsync(bool isManual = false, std::function<void(const UpdateInfo&)> callback = nullptr);
    UpdateInfo CheckUpdatesSync(const std::string& customChannelOrUrl = "");
    
    UpdateInfo GetLastUpdateInfo();
    std::string GetCurrentAppVersion();
    bool IsNewerVersion(const std::string& currentVer, const std::string& remoteVer);
    void OpenDownloadUrl();
    void OpenReleasePage();

private:
    UpdateManager();
    ~UpdateManager() = default;
    UpdateManager(const UpdateManager&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;

    std::string FetchUrlContent(const std::string& url);
    UpdateInfo ParseTelegramChannel(const std::string& htmlContent, const std::string& channelName);
    UpdateInfo ParseTgIcaJson(const std::string& jsonContent, const std::string& channelName);
    UpdateInfo ParseGitHubRelease(const std::string& jsonContent, const std::string& repoPath);
    UpdateInfo ParseGitHubHtmlReleases(const std::string& htmlContent, const std::string& repoPath);
    UpdateInfo ParseJsonFeed(const std::string& jsonContent);

    std::mutex m_mutex;
    UpdateInfo m_lastInfo;
    std::string m_cachedCurrentVersion;
    bool m_checking = false;
    bool m_initialized = false;
};

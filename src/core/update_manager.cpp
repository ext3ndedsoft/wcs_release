#include "update_manager.h"
#include "common_utils.h"
#include "config_manager.h"
#include "native_bridge.h"
#include "app_version.h"
#include <wininet.h>
#include <shellapi.h>
#include <regex>
#include <sstream>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "version.lib")

static std::string StripHtmlTags(std::string text) {
    // Replace <br> and <br/> with newline
    std::regex brRegex(R"(<br\s*/?>)", std::regex_constants::icase);
    text = std::regex_replace(text, brRegex, "\n");

    // Replace common HTML entities
    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t start = 0;
        while ((start = str.find(from, start)) != std::string::npos) {
            str.replace(start, from.length(), to);
            start += to.length();
        }
    };
    replaceAll(text, "&quot;", "\"");
    replaceAll(text, "&#39;", "'");
    replaceAll(text, "&apos;", "'");
    replaceAll(text, "&amp;", "&");
    replaceAll(text, "&lt;", "<");
    replaceAll(text, "&gt;", ">");
    replaceAll(text, "&nbsp;", " ");

    // Remove remaining HTML tags
    std::regex tagRegex(R"(<[^>]+>)");
    text = std::regex_replace(text, tagRegex, "");

    // Trim excessive blank lines
    std::regex multiNewline(R"(\n{3,})");
    text = std::regex_replace(text, multiNewline, "\n\n");

    // Trim leading/trailing whitespace
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, (last - first + 1));
}

// -----------------------------------------------------------------------------
// Semantic Version Engine (Supports any version up to 15.0+ and beyond)
// -----------------------------------------------------------------------------
struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    int build = 0;
    bool valid = false;

    bool operator>(const SemVer& o) const {
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        if (patch != o.patch) return patch > o.patch;
        return build > o.build;
    }

    bool operator<=(const SemVer& o) const {
        return !(*this > o);
    }

    bool operator<(const SemVer& o) const {
        return o > *this;
    }

    bool operator>=(const SemVer& o) const {
        return !(*this < o);
    }

    bool operator==(const SemVer& o) const {
        return major == o.major && minor == o.minor && patch == o.patch && build == o.build;
    }

    std::string ToString() const {
        std::string s = std::to_string(major) + "." + std::to_string(minor);
        if (patch > 0 || build > 0) {
            s += "." + std::to_string(patch);
            if (build > 0) {
                s += "." + std::to_string(build);
            }
        }
        return s;
    }
};

static SemVer ParseSemVer(const std::string& input) {
    SemVer ver;
    if (input.empty()) return ver;

    // Strip any occurrence of "Pro", "PRO", "pro", "v", "V" before parsing digits
    std::string str = input;
    std::regex proRegex(R"(pro)", std::regex_constants::icase);
    str = std::regex_replace(str, proRegex, " ");

    std::regex vRegex(R"([vV])");
    str = std::regex_replace(str, vRegex, " ");

    // 1. Primary: match dotted semver numbers like 2.0, 2.1, 15.0, 15.0.1, v15.0.0.1
    // Ignores architecture numbers like x64, Windows 11, etc.
    std::regex semverRegex(R"((?:^|[^\d\.])(?:v|ver|version|версия)?\s*(\d{1,5})\.(\d{1,5})(?:\.(\d{1,5}))?(?:\.(\d{1,5}))?(?:[^\d\.]|$))", std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(str, match, semverRegex)) {
        try {
            ver.major = std::stoi(match[1].str());
            ver.minor = std::stoi(match[2].str());
            ver.patch = match[3].matched ? std::stoi(match[3].str()) : 0;
            ver.build = match[4].matched ? std::stoi(match[4].str()) : 0;
            ver.valid = true;
            return ver;
        } catch (...) {}
    }

    // 2. Secondary: single version number with prefix e.g. "v15", "Release 15", "Версия 15"
    std::regex singleVerRegex(R"((?:^|[^\w])(?:v|ver|version|версия|release|релиз)\s*[:=]?\s*(\d{1,5})(?:[^\d]|$))", std::regex_constants::icase);
    if (std::regex_search(str, match, singleVerRegex)) {
        try {
            ver.major = std::stoi(match[1].str());
            ver.minor = 0;
            ver.patch = 0;
            ver.build = 0;
            ver.valid = true;
            return ver;
        } catch (...) {}
    }

    // 3. Fallback: input is purely an integer tag e.g. "15" or "v15"
    std::regex pureNumRegex(R"(^\s*v?(\d{1,5})\s*$)");
    if (std::regex_search(str, match, pureNumRegex)) {
        try {
            ver.major = std::stoi(match[1].str());
            ver.minor = 0;
            ver.patch = 0;
            ver.build = 0;
            ver.valid = true;
            return ver;
        } catch (...) {}
    }

    return ver;
}

std::string UpdateManager::GetCurrentAppVersion() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cachedCurrentVersion.empty()) {
        return m_cachedCurrentVersion;
    }

    // 1. Dynamically query PE VERSIONINFO from running executable module
    wchar_t exePath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
        DWORD dummy = 0;
        DWORD size = GetFileVersionInfoSizeW(exePath, &dummy);
        if (size > 0) {
            std::vector<BYTE> data(size);
            if (GetFileVersionInfoW(exePath, 0, size, data.data())) {
                VS_FIXEDFILEINFO* pFileInfo = nullptr;
                UINT len = 0;
                if (VerQueryValueW(data.data(), L"\\", (LPVOID*)&pFileInfo, &len) && pFileInfo && len >= sizeof(VS_FIXEDFILEINFO)) {
                    int major = HIWORD(pFileInfo->dwProductVersionMS);
                    int minor = LOWORD(pFileInfo->dwProductVersionMS);
                    int patch = HIWORD(pFileInfo->dwProductVersionLS);
                    if (major > 0 || minor > 0) {
                        std::string ver = std::to_string(major) + "." + std::to_string(minor);
                        if (patch > 0) {
                            ver += "." + std::to_string(patch);
                        }
                        m_cachedCurrentVersion = ver;
                        m_lastInfo.currentVersion = ver;
                        return m_cachedCurrentVersion;
                    }
                }
            }
        }
    }

    // 2. Fallback to centralized header constant
    m_cachedCurrentVersion = WCS_DISPLAY_VERSION;
    m_lastInfo.currentVersion = m_cachedCurrentVersion;
    return m_cachedCurrentVersion;
}

UpdateManager::UpdateManager() {
    m_lastInfo.currentVersion = GetCurrentAppVersion();
}

UpdateManager& UpdateManager::Get() {
    static UpdateManager instance;
    return instance;
}

void UpdateManager::Init() {
    if (m_initialized) return;
    m_initialized = true;

    // Automatic update check on startup is permanently active in open-source release
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        CheckUpdatesAsync(false, [](const UpdateInfo& info) {
            if (info.available) {
                nlohmann::json data;
                data["version"] = info.latestVersion;
                data["latestVersion"] = info.latestVersion;
                data["currentVersion"] = info.currentVersion;
                data["url"] = info.downloadUrl;
                data["downloadUrl"] = info.downloadUrl;
                data["releaseUrl"] = info.releaseUrl;
                data["changelog"] = info.changelog;
                data["date"] = info.publishDate;
                data["channel"] = info.channel;
                NativeBridge::Get().BroadcastEvent("update_available", data);
            }
        });
    }).detach();
}

bool UpdateManager::IsNewerVersion(const std::string& currentVer, const std::string& remoteVer) {
    SemVer cur = ParseSemVer(currentVer);
    SemVer rem = ParseSemVer(remoteVer);

    if (!rem.valid || !cur.valid) return false;

    // Strict greater-than check: ONLY return true if remote is strictly newer!
    return rem > cur;
}

static std::string FetchUrlViaCurlProcess(const std::wstring& url) {
    std::wstring cmd = L"\"C:\\Windows\\System32\\curl.exe\" -s -k -L --connect-timeout 5 -m 8 "
                       L"-H \"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\" "
                       L"-H \"Accept: application/vnd.github.v3+json\" \"" + url + L"\"";
    
    ScopedHandle hReadPipe;
    ScopedHandle hWritePipe;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(hReadPipe.put(), hWritePipe.put(), &sa, 0)) return "";
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi = { 0 };
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (!CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return "";
    }
    ScopedHandle hProc(pi.hProcess);
    ScopedHandle hThrd(pi.hThread);

    hWritePipe.Close();

    std::string result;
    char buffer[8192];
    DWORD bytesRead = 0;
    while (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
        result.append(buffer, bytesRead);
        if (result.size() > 4 * 1024 * 1024) break;
    }

    WaitForSingleObject(hProc, 8000);
    return result;
}

std::string UpdateManager::FetchUrlContent(const std::string& url) {
    std::wstring urlW = Utf8ToWideStr(url);
    ScopedHInternet hInternet(InternetOpenW(
        L"WindowsConfigStudio-Updater/3.0",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0));
    
    std::string response;

    if (hInternet) {
        DWORD timeoutMs = 4000;
        InternetSetOptionW(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionW(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
        InternetSetOptionW(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
        if (url.rfind("https://", 0) == 0) {
            flags |= INTERNET_FLAG_SECURE;
        }

        const wchar_t* customHeaders = L"User-Agent: WindowsConfigStudio-Updater/3.0\r\nAccept: application/vnd.github.v3+json\r\n";
        ScopedHInternet hUrl(InternetOpenUrlW(hInternet, urlW.c_str(), customHeaders, (DWORD)-1L, flags, 0));
        if (hUrl) {
            DWORD statusCode = 0;
            DWORD statusCodeSize = sizeof(statusCode);
            bool statusOk = true;
            if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusCodeSize, NULL)) {
                if (statusCode >= 400) {
                    statusOk = false;
                }
            }

            if (statusOk) {
                char buffer[8192];
                DWORD bytesRead = 0;
                while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    response.append(buffer, bytesRead);
                    if (response.size() > 4 * 1024 * 1024) break; // 4MB limit
                }
            }
        }
    }

    // If WinINet failed or timed out (e.g. IPv6 connection timeout on Russian ISPs), seamlessly fall back to native silent curl process
    if (response.empty()) {
        response = FetchUrlViaCurlProcess(urlW);
    }

    return response;
}

UpdateInfo UpdateManager::ParseJsonFeed(const std::string& jsonContent) {
    UpdateInfo info;
    info.currentVersion = GetCurrentAppVersion();

    try {
        auto j = nlohmann::json::parse(jsonContent);
        if (j.contains("version")) {
            info.latestVersion = j["version"].get<std::string>();
            SemVer sv = ParseSemVer(info.latestVersion);
            if (sv.valid) {
                info.latestVersion = sv.ToString();
            }
        }
        if (j.contains("url")) {
            info.downloadUrl = j["url"].get<std::string>();
        } else if (j.contains("downloadUrl")) {
            info.downloadUrl = j["downloadUrl"].get<std::string>();
        }
        if (j.contains("changelog")) {
            info.changelog = j["changelog"].get<std::string>();
        } else if (j.contains("notes")) {
            info.changelog = j["notes"].get<std::string>();
        }
        if (j.contains("date")) {
            info.publishDate = j["date"].get<std::string>();
        }
        info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
    } catch (...) {
        info.errorMsg = "Ошибка разбора JSON-ответа обновления";
    }

    return info;
}

UpdateInfo UpdateManager::ParseTelegramChannel(const std::string& htmlContent, const std::string& channelName) {
    UpdateInfo info;
    info.currentVersion = GetCurrentAppVersion();
    info.channel = channelName;

    if (htmlContent.empty()) {
        info.errorMsg = "Не удалось подключиться к каналу Telegram или канал недоступен";
        return info;
    }

    // Telegram public channel web preview (/s/) renders messages in divs:
    // <div class="tgme_widget_message_text ..."> ... </div>
    // Let's find all message text blocks
    std::regex msgRegex(R"(<div[^>]*class="[^"]*tgme_widget_message_text[^"]*"[^>]*>([\s\S]*?)</div>)");
    auto words_begin = std::sregex_iterator(htmlContent.begin(), htmlContent.end(), msgRegex);
    auto words_end = std::sregex_iterator();

    std::vector<std::string> messages;
    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        if (match.size() > 1) {
            messages.push_back(match[1].str());
        }
    }

    if (messages.empty()) {
        info.errorMsg = "В Telegram-канале не найдено сообщений с релизами";
        return info;
    }

    // Search from the newest message (last in HTML) to older messages
    for (int i = (int)messages.size() - 1; i >= 0; --i) {
        std::string rawMsg = messages[i];

        // Check if raw message contains JSON block
        size_t jsonStart = rawMsg.find("{");
        size_t jsonEnd = rawMsg.rfind("}");
        if (jsonStart != std::string::npos && jsonEnd != std::string::npos && jsonEnd > jsonStart) {
            std::string possibleJson = StripHtmlTags(rawMsg.substr(jsonStart, jsonEnd - jsonStart + 1));
            try {
                auto j = nlohmann::json::parse(possibleJson);
                if (j.contains("version") && (j.contains("url") || j.contains("downloadUrl"))) {
                    info.latestVersion = j["version"].get<std::string>();
                    SemVer sv = ParseSemVer(info.latestVersion);
                    if (sv.valid) info.latestVersion = sv.ToString();
                    info.downloadUrl = j.contains("url") ? j["url"].get<std::string>() : j["downloadUrl"].get<std::string>();
                    info.changelog = j.contains("changelog") ? j["changelog"].get<std::string>() : (j.contains("notes") ? j["notes"].get<std::string>() : "");
                    info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
                    return info;
                }
            } catch (...) {}
        }

        std::string cleanText = StripHtmlTags(rawMsg);

        // Pattern 1: Версия / Version / #release / Релиз / vX.Y
        std::regex verRegex(R"((?:\b(?:Версия|Version|Release|Релиз|#release)\b|[#vV])\s*[:=]?\s*([vV]?\s*\d+(?:\.\d+)+(?:[a-zA-Z0-9_\-\.]*)))", std::regex_constants::icase);
        std::smatch verMatch;
        if (std::regex_search(cleanText, verMatch, verRegex)) {
            info.latestVersion = verMatch[1].str();
            SemVer sv = ParseSemVer(info.latestVersion);
            if (sv.valid) {
                info.latestVersion = sv.ToString();
            } else {
                while (!info.latestVersion.empty() && (info.latestVersion.front() == 'v' || info.latestVersion.front() == 'V' || info.latestVersion.front() == ' ')) {
                    info.latestVersion.erase(info.latestVersion.begin());
                }
                while (!info.latestVersion.empty() && (info.latestVersion.back() == ' ' || info.latestVersion.back() == '\r')) {
                    info.latestVersion.pop_back();
                }
            }

            // Pattern 2: Download link
            std::regex urlRegex(R"((?:Ссылка|Скачать|URL|Download)\s*[:=]?\s*(https?://[^\s<"]+))", std::regex_constants::icase);
            std::smatch urlMatch;
            if (std::regex_search(cleanText, urlMatch, urlRegex)) {
                info.downloadUrl = urlMatch[1].str();
            } else {
                // Search for any https link
                std::regex genericUrlRegex(R"(https?://[^\s<"]+)");
                std::smatch genMatch;
                if (std::regex_search(cleanText, genMatch, genericUrlRegex)) {
                    info.downloadUrl = genMatch[0].str();
                } else {
                    // Fallback to GitHub releases repo
                    info.downloadUrl = "https://github.com/ext3ndedsoft/wcs_release";
                }
            }

            // Pattern 3: Changelog
            std::regex clRegex(R"((?:Что нового|Чейнджлог|Changelog|Изменения|Описание)\s*[:=]?\s*([\s\S]*))", std::regex_constants::icase);
            std::smatch clMatch;
            if (std::regex_search(cleanText, clMatch, clRegex)) {
                info.changelog = clMatch[1].str();
            } else {
                // Fallback: entire text after the version line
                size_t vPos = cleanText.find(info.latestVersion);
                if (vPos != std::string::npos) {
                    size_t nextLine = cleanText.find('\n', vPos);
                    if (nextLine != std::string::npos) {
                        info.changelog = cleanText.substr(nextLine + 1);
                    }
                }
            }

            info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
            return info;
        }
    }

    info.errorMsg = "Не удалось распознать формат релиза в последних сообщениях канала";
    return info;
}

UpdateInfo UpdateManager::ParseTgIcaJson(const std::string& jsonContent, const std::string& channelName) {
    UpdateInfo info;
    info.currentVersion = GetCurrentAppVersion();
    info.channel = "https://t.me/" + channelName;

    try {
        auto j = nlohmann::json::parse(jsonContent);
        if (j.contains("error")) {
            info.errorMsg = j["error"].is_string() ? j["error"].get<std::string>() : "Ошибка шлюза Telegram";
            return info;
        }

        if (!j.contains("messages") || !j["messages"].is_array()) {
            info.errorMsg = "Неверный формат ответа шлюза Telegram";
            return info;
        }

        const auto& msgs = j["messages"];
        // Iterate through messages (first ones are the newest)
        for (const auto& item : msgs) {
            if (!item.contains("message") || !item["message"].is_string()) continue;
            std::string rawMsg = item["message"].get<std::string>();
            if (rawMsg.empty()) continue;

            // Extract date if present
            if (item.contains("date") && item["date"].is_number()) {
                time_t t = item["date"].get<time_t>();
                char dateBuf[64] = { 0 };
                struct tm tm_buf;
                localtime_s(&tm_buf, &t);
                strftime(dateBuf, sizeof(dateBuf), "%d.%m.%Y %H:%M", &tm_buf);
                info.publishDate = dateBuf;
            }

            // Version regex: "Версия: 2.1" or "v2.1" or "Version: 2.1" or "Релиз v2.1"
            std::regex verRegex(R"((?:\b(?:Версия|Version|Release|Релиз|#release)\b|[#vV])\s*[:=]?\s*([vV]?\s*\d+(?:\.\d+)+(?:[a-zA-Z0-9_\-\.]*)))", std::regex_constants::icase);
            std::smatch verMatch;
            if (std::regex_search(rawMsg, verMatch, verRegex)) {
                info.latestVersion = verMatch[1].str();
                SemVer sv = ParseSemVer(info.latestVersion);
                if (sv.valid) {
                    info.latestVersion = sv.ToString();
                } else {
                    while (!info.latestVersion.empty() && (info.latestVersion.front() == 'v' || info.latestVersion.front() == 'V' || info.latestVersion.front() == ' ')) {
                        info.latestVersion.erase(info.latestVersion.begin());
                    }
                    while (!info.latestVersion.empty() && (info.latestVersion.back() == ' ' || info.latestVersion.back() == '\r')) {
                        info.latestVersion.pop_back();
                    }
                }

                // Download Link
                std::regex urlRegex(R"((?:Ссылка|Скачать|URL|Download)\s*[:=]?\s*(https?://[^\s<"]+))", std::regex_constants::icase);
                std::smatch urlMatch;
                if (std::regex_search(rawMsg, urlMatch, urlRegex)) {
                    info.downloadUrl = urlMatch[1].str();
                } else {
                    std::regex genericUrl(R"(https?://[^\s<"]+)");
                    std::smatch genMatch;
                    if (std::regex_search(rawMsg, genMatch, genericUrl)) {
                        info.downloadUrl = genMatch[0].str();
                    } else {
                        info.downloadUrl = "https://t.me/" + channelName;
                    }
                }

                // Changelog
                std::regex clRegex(R"((?:Что нового|Чейнджлог|Changelog|Изменения|Описание)\s*[:=]?\s*([\s\S]*))", std::regex_constants::icase);
                std::smatch clMatch;
                if (std::regex_search(rawMsg, clMatch, clRegex)) {
                    info.changelog = clMatch[1].str();
                } else {
                    size_t vPos = rawMsg.find(info.latestVersion);
                    if (vPos != std::string::npos) {
                        size_t nextLine = rawMsg.find('\n', vPos);
                        if (nextLine != std::string::npos) {
                            info.changelog = rawMsg.substr(nextLine + 1);
                        }
                    }
                }

                // Trim leading/trailing whitespace
                size_t first = info.changelog.find_first_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    size_t last = info.changelog.find_last_not_of(" \t\r\n");
                    info.changelog = info.changelog.substr(first, (last - first + 1));
                }

                info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
                return info;
            }
        }
    } catch (const std::exception& e) {
        info.errorMsg = std::string("Ошибка JSON Telegram: ") + e.what();
        return info;
    }

    info.errorMsg = "В Telegram-канале не найдено постов с номером версии";
    return info;
}

UpdateInfo UpdateManager::ParseGitHubRelease(const std::string& jsonContent, const std::string& repoPath) {
    UpdateInfo info;
    info.currentVersion = GetCurrentAppVersion();
    info.channel = "https://github.com/" + repoPath;

    try {
        auto j = nlohmann::json::parse(jsonContent);
        if (j.is_array()) {
            if (!j.empty()) {
                j = j[0];
            } else {
                info.errorMsg = "Список релизов GitHub пуст";
                return info;
            }
        }

        if (j.contains("message") && !j.contains("tag_name") && !j.contains("name")) {
            info.errorMsg = j["message"].is_string() ? j["message"].get<std::string>() : "Ошибка GitHub API";
            return info;
        }

        std::string releaseName = j.value("name", "");
        std::string releaseTag = j.value("tag_name", "");
        SemVer svName = ParseSemVer(releaseName);
        SemVer svTag = ParseSemVer(releaseTag);
        if (svName.valid) {
            info.latestVersion = svName.ToString();
        } else if (svTag.valid) {
            info.latestVersion = svTag.ToString();
        } else if (!releaseName.empty()) {
            info.latestVersion = releaseName;
        } else {
            info.latestVersion = releaseTag;
        }

        if (!svName.valid && !svTag.valid) {
            while (!info.latestVersion.empty() && (info.latestVersion.front() == 'v' || info.latestVersion.front() == 'V' || info.latestVersion.front() == ' ')) {
                info.latestVersion.erase(info.latestVersion.begin());
            }
            while (!info.latestVersion.empty() && (info.latestVersion.back() == ' ' || info.latestVersion.back() == '\r')) {
                info.latestVersion.pop_back();
            }
        }

        // Direct binary download link if attached in assets (.exe or .zip)
        if (j.contains("assets") && j["assets"].is_array() && !j["assets"].empty()) {
            for (const auto& asset : j["assets"]) {
                if (asset.contains("browser_download_url")) {
                    std::string aName = asset.value("name", "");
                    if (aName.find(".exe") != std::string::npos) {
                        info.downloadUrl = asset["browser_download_url"].get<std::string>();
                        break;
                    } else if (aName.find(".zip") != std::string::npos || info.downloadUrl.empty()) {
                        info.downloadUrl = asset["browser_download_url"].get<std::string>();
                    }
                }
            }
        }
        if (j.contains("html_url") && j["html_url"].is_string()) {
            info.releaseUrl = j["html_url"].get<std::string>();
        } else {
            info.releaseUrl = "https://github.com/" + repoPath + "/releases";
        }

        if (info.downloadUrl.empty()) {
            if (!releaseTag.empty()) {
                info.downloadUrl = "https://github.com/" + repoPath + "/releases/download/" + releaseTag + "/WCS.exe";
            } else {
                info.downloadUrl = info.releaseUrl;
            }
        }

        if (j.contains("body") && j["body"].is_string()) {
            info.changelog = j["body"].get<std::string>();
        }

        if (j.contains("published_at") && j["published_at"].is_string()) {
            std::string pub = j["published_at"].get<std::string>();
            if (pub.size() >= 10) {
                std::string y = pub.substr(0, 4);
                std::string m = pub.substr(5, 2);
                std::string d = pub.substr(8, 2);
                info.publishDate = d + "." + m + "." + y;
            }
        }

        info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
        return info;
    } catch (const std::exception& e) {
        info.errorMsg = std::string("Ошибка JSON GitHub: ") + e.what();
        return info;
    }
}

UpdateInfo UpdateManager::ParseGitHubHtmlReleases(const std::string& htmlContent, const std::string& repoPath) {
    UpdateInfo info;
    info.currentVersion = GetCurrentAppVersion();
    info.channel = "https://github.com/" + repoPath;

    if (htmlContent.empty()) {
        info.errorMsg = "HTML-страница релизов GitHub пуста";
        return info;
    }

    std::string releaseTag;
    std::string releaseName;

    // Search for release tag link
    std::regex tagLinkRegex(R"(href=[\"\'][^\"\']*\/releases\/tag\/([^\"\'\s>]+)[\"\'][^>]*>([\s\S]*?)<\/a>)", std::regex_constants::icase);
    std::smatch tagMatch;
    if (std::regex_search(htmlContent, tagMatch, tagLinkRegex)) {
        releaseTag = tagMatch[1].str();
        releaseName = StripHtmlTags(tagMatch[2].str());
    } else {
        std::regex simpleTagRegex(R"(\/releases\/tag\/([^\"\'\s>]+))", std::regex_constants::icase);
        if (std::regex_search(htmlContent, tagMatch, simpleTagRegex)) {
            releaseTag = tagMatch[1].str();
        }
    }

    if (releaseName.empty() && !releaseTag.empty()) {
        releaseName = releaseTag;
    }

    SemVer svName = ParseSemVer(releaseName);
    SemVer svTag = ParseSemVer(releaseTag);
    if (svName.valid) {
        info.latestVersion = svName.ToString();
    } else if (svTag.valid) {
        info.latestVersion = svTag.ToString();
    } else if (!releaseName.empty()) {
        info.latestVersion = releaseName;
    } else {
        info.latestVersion = releaseTag;
    }

    if (!svName.valid && !svTag.valid) {
        while (!info.latestVersion.empty() && (info.latestVersion.front() == 'v' || info.latestVersion.front() == 'V' || info.latestVersion.front() == ' ')) {
            info.latestVersion.erase(info.latestVersion.begin());
        }
        while (!info.latestVersion.empty() && (info.latestVersion.back() == ' ' || info.latestVersion.back() == '\r')) {
            info.latestVersion.pop_back();
        }
    }

    // Direct binary download link if found in HTML
    std::regex dlRegex(R"(href=[\"\']([^\"\']*\/releases\/download\/[^\"\']+)[\"\'])", std::regex_constants::icase);
    std::smatch dlMatch;
    if (std::regex_search(htmlContent, dlMatch, dlRegex)) {
        std::string dl = dlMatch[1].str();
        if (dl.rfind("http", 0) != 0) {
            dl = "https://github.com" + dl;
        }
        info.downloadUrl = dl;
    } else if (!releaseTag.empty()) {
        info.downloadUrl = "https://github.com/" + repoPath + "/releases/download/" + releaseTag + "/WCS.exe";
    } else {
        info.downloadUrl = "https://github.com/" + repoPath + "/releases";
    }

    if (!releaseTag.empty()) {
        info.releaseUrl = "https://github.com/" + repoPath + "/releases/tag/" + releaseTag;
    } else {
        info.releaseUrl = "https://github.com/" + repoPath + "/releases";
    }

    std::regex bodyRegex(R"(class=[\"\'][^\"\']*markdown-body[^\"\']*[\"\'][^>]*>([\s\S]*?)<\/div>)", std::regex_constants::icase);
    std::smatch bodyMatch;
    if (std::regex_search(htmlContent, bodyMatch, bodyRegex)) {
        info.changelog = StripHtmlTags(bodyMatch[1].str());
    } else {
        info.changelog = "Официальный релиз Windows Config Studio на GitHub Releases.";
    }

    info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
    return info;
}

UpdateInfo UpdateManager::CheckUpdatesSync(const std::string& customChannelOrUrl) {
    std::string channelOrUrl = customChannelOrUrl;
    if (channelOrUrl.empty()) {
        channelOrUrl = ConfigManager::Get().GetConfig().updateChannel;
    }
    if (channelOrUrl.empty()) {
        channelOrUrl = "https://github.com/ext3ndedsoft/wcs_release";
    }

    UpdateInfo info;
    info.currentVersion = GetCurrentAppVersion();
    info.channel = channelOrUrl;

    // 1. Check if channelOrUrl represents a GitHub repository
    std::string ghRepo;
    if (channelOrUrl.find("github.com/") != std::string::npos) {
        size_t p = channelOrUrl.find("github.com/");
        ghRepo = channelOrUrl.substr(p + 11);
    } else if (channelOrUrl.find('/') != std::string::npos && channelOrUrl.find("t.me") == std::string::npos && channelOrUrl.rfind("http", 0) != 0) {
        // e.g. "ecst4ssy/WindowsConfigStudio"
        ghRepo = channelOrUrl;
    }

    if (!ghRepo.empty()) {
        size_t rel = ghRepo.find("/releases");
        if (rel != std::string::npos) ghRepo = ghRepo.substr(0, rel);
        if (ghRepo.size() > 4 && ghRepo.rfind(".git") == ghRepo.size() - 4) {
            ghRepo = ghRepo.substr(0, ghRepo.size() - 4);
        }
        while (!ghRepo.empty() && (ghRepo.back() == '/' || ghRepo.back() == ' ')) {
            ghRepo.pop_back();
        }

        // 1. Try /releases/latest endpoint
        std::string ghApiUrl = "https://api.github.com/repos/" + ghRepo + "/releases/latest";
        std::string ghContent = FetchUrlContent(ghApiUrl);
        if (!ghContent.empty()) {
            info = ParseGitHubRelease(ghContent, ghRepo);
        }

        // 2. If empty or error, try /releases endpoint (where item 0 is the latest release)
        if (info.latestVersion.empty() || !info.errorMsg.empty()) {
            std::string ghApiReleasesUrl = "https://api.github.com/repos/" + ghRepo + "/releases";
            std::string ghReleasesContent = FetchUrlContent(ghApiReleasesUrl);
            if (!ghReleasesContent.empty()) {
                info = ParseGitHubRelease(ghReleasesContent, ghRepo);
            }
        }

        // 3. If both return empty (e.g. rate-limiting), parse HTML releases page
        if (info.latestVersion.empty() || !info.errorMsg.empty()) {
            std::string ghHtmlUrl = "https://github.com/" + ghRepo + "/releases";
            std::string ghHtmlContent = FetchUrlContent(ghHtmlUrl);
            if (!ghHtmlContent.empty()) {
                info = ParseGitHubHtmlReleases(ghHtmlContent, ghRepo);
            }
        }

        if (!info.latestVersion.empty()) {
            info.errorMsg.clear();
            if (info.publishDate.empty()) {
                auto now = std::chrono::system_clock::now();
                std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                char timeBuf[64] = { 0 };
                struct tm tm_buf;
                localtime_s(&tm_buf, &now_time);
                strftime(timeBuf, sizeof(timeBuf), "%d.%m.%Y %H:%M", &tm_buf);
                info.publishDate = timeBuf;
            }
            // Strict version guard: if remote version matches or is older, available MUST be false!
            info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastInfo = info;
            return info;
        }

        info.errorMsg = "Не удалось получить релиз с GitHub (проверьте репозиторий)";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastInfo = info;
        return info;
    }

    // Check if channelOrUrl represents a Telegram channel
    std::string channelName;
    if (channelOrUrl.find("t.me/") != std::string::npos) {
        size_t p = channelOrUrl.find("t.me/s/");
        if (p != std::string::npos) {
            channelName = channelOrUrl.substr(p + 7);
        } else {
            p = channelOrUrl.find("t.me/");
            channelName = channelOrUrl.substr(p + 5);
        }
    } else if (!channelOrUrl.empty() && channelOrUrl.front() == '@') {
        channelName = channelOrUrl.substr(1);
    } else if (channelOrUrl.rfind("http://", 0) != 0 && channelOrUrl.rfind("https://", 0) != 0) {
        channelName = channelOrUrl;
    }

    // Clean any trailing slashes, spaces or queries from channelName
    while (!channelName.empty() && (channelName.back() == '/' || channelName.back() == ' ' || channelName.back() == '\r')) {
        channelName.pop_back();
    }
    size_t qPos = channelName.find('?');
    if (qPos != std::string::npos) {
        channelName = channelName.substr(0, qPos);
    }

    if (!channelName.empty()) {
        // 1. Primary endpoint: tg.i-c-a.su REST JSON gateway (unblocked in Russia, ultra-fast)
        std::string jsonUrl = "https://tg.i-c-a.su/json/" + channelName;
        std::string jsonContent = FetchUrlContent(jsonUrl);
        if (!jsonContent.empty()) {
            info = ParseTgIcaJson(jsonContent, channelName);
            if (!info.latestVersion.empty()) {
                if (info.publishDate.empty()) {
                    auto now = std::chrono::system_clock::now();
                    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                    char timeBuf[64] = { 0 };
                    struct tm tm_buf;
                    localtime_s(&tm_buf, &now_time);
                    strftime(timeBuf, sizeof(timeBuf), "%d.%m.%Y %H:%M", &tm_buf);
                    info.publishDate = timeBuf;
                }
                info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastInfo = info;
                return info;
            }
        }

        // 2. Secondary fallback: official t.me/s/ web preview
        std::string webUrl = "https://t.me/s/" + channelName;
        std::string htmlContent = FetchUrlContent(webUrl);
        if (!htmlContent.empty()) {
            info = ParseTelegramChannel(htmlContent, channelName);
            if (!info.latestVersion.empty()) {
                if (info.publishDate.empty()) {
                    auto now = std::chrono::system_clock::now();
                    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
                    char timeBuf[64] = { 0 };
                    struct tm tm_buf;
                    localtime_s(&tm_buf, &now_time);
                    strftime(timeBuf, sizeof(timeBuf), "%d.%m.%Y %H:%M", &tm_buf);
                    info.publishDate = timeBuf;
                }
                info.available = IsNewerVersion(info.currentVersion, info.latestVersion);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastInfo = info;
                return info;
            }
        }

        info.errorMsg = "Нет соединения с сервером обновлений или канал недоступен";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastInfo = info;
        return info;
    }

    // Direct JSON feed endpoint (e.g. GitHub Releases raw JSON or custom URL)
    std::string content = FetchUrlContent(channelOrUrl);
    if (content.empty()) {
        info.errorMsg = "Нет соединения с сервером обновлений";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastInfo = info;
        return info;
    }

    info = ParseJsonFeed(content);
    if (info.publishDate.empty()) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        char timeBuf[64] = { 0 };
        struct tm tm_buf;
        localtime_s(&tm_buf, &now_time);
        strftime(timeBuf, sizeof(timeBuf), "%d.%m.%Y %H:%M", &tm_buf);
        info.publishDate = timeBuf;
    }
    info.available = IsNewerVersion(info.currentVersion, info.latestVersion);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastInfo = info;
    }

    return info;
}

void UpdateManager::CheckUpdatesAsync(bool isManual, std::function<void(const UpdateInfo&)> callback) {
    std::thread([this, isManual, callback]() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_checking) return;
            m_checking = true;
        }

        UpdateInfo info = CheckUpdatesSync();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_checking = false;
        }

        if (callback) {
            callback(info);
        }
    }).detach();
}

UpdateInfo UpdateManager::GetLastUpdateInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastInfo;
}

void UpdateManager::OpenDownloadUrl() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        url = m_lastInfo.downloadUrl;
        if (url.empty()) {
            url = m_lastInfo.releaseUrl;
        }
        if (url.empty()) {
            std::string ch = m_lastInfo.channel;
            if (ch.empty()) ch = "https://github.com/ext3ndedsoft/wcs_release";
            if (ch.rfind("http", 0) == 0) {
                url = ch;
            } else {
                url = "https://github.com/" + ch;
            }
        }
    }

    if (!url.empty()) {
        std::wstring wUrl = Utf8ToWideStr(url);
        ShellExecuteW(NULL, L"open", wUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
}

void UpdateManager::OpenReleasePage() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        url = m_lastInfo.releaseUrl;
        if (url.empty()) {
            std::string ch = m_lastInfo.channel;
            if (ch.empty()) ch = "https://github.com/ext3ndedsoft/wcs_release/releases";
            if (ch.rfind("http", 0) == 0) {
                url = ch;
            } else {
                url = "https://github.com/" + ch + "/releases";
            }
        }
    }

    if (!url.empty()) {
        std::wstring wUrl = Utf8ToWideStr(url);
        ShellExecuteW(NULL, L"open", wUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
}

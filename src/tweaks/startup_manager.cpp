#include "startup_manager.h"
#include "common_utils.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "shlwapi.lib")

static void TrimString(std::wstring& s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) {
        s.pop_back();
    }
}

static bool ResolveShortcut(const std::wstring& lnkPath, std::wstring& outTarget, std::wstring& outArgs) {
    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)shellLink.GetAddressOf());
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    hr = shellLink.As(&persistFile);
    if (FAILED(hr)) return false;

    hr = persistFile->Load(lnkPath.c_str(), STGM_READ);
    if (FAILED(hr)) return false;

    wchar_t target[MAX_PATH] = { 0 };
    WIN32_FIND_DATAW wfd = { 0 };
    hr = shellLink->GetPath(target, MAX_PATH, &wfd, SLGP_UNCPRIORITY);
    if (SUCCEEDED(hr)) {
        outTarget = target;
    }

    wchar_t args[1024] = { 0 };
    if (SUCCEEDED(shellLink->GetArguments(args, 1024))) {
        outArgs = args;
    }
    return true;
}

static void ResolveExecutable(const std::wstring& cmd, std::wstring& outExe, std::string& outPublisher, bool& outExists) {
    std::wstring s = cmd;
    TrimString(s);

    if (s.empty()) {
        outExe = L"";
        outPublisher = "Не указан";
        outExists = false;
        return;
    }

    // 1. Extract target executable path (handles quotes and command line args)
    if (s.front() == L'"') {
        size_t endQuote = s.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            outExe = s.substr(1, endQuote - 1);
        } else {
            outExe = s.substr(1);
        }
    } else {
        std::wstring lower = s;
        for (auto& c : lower) c = towlower(c);

        size_t exePos = lower.find(L".exe");
        if (exePos != std::wstring::npos) {
            outExe = s.substr(0, exePos + 4);
        } else {
            size_t batPos = lower.find(L".bat");
            if (batPos != std::wstring::npos) {
                outExe = s.substr(0, batPos + 4);
            } else {
                size_t cmdPos = lower.find(L".cmd");
                if (cmdPos != std::wstring::npos) {
                    outExe = s.substr(0, cmdPos + 4);
                } else {
                    size_t spacePos = s.find(L' ');
                    if (spacePos != std::wstring::npos) {
                        outExe = s.substr(0, spacePos);
                    } else {
                        outExe = s;
                    }
                }
            }
        }
    }

    TrimString(outExe);

    // Expand environment variables (e.g. %SystemRoot%, %ProgramFiles%)
    wchar_t expanded[MAX_PATH * 2] = { 0 };
    if (ExpandEnvironmentStringsW(outExe.c_str(), expanded, ARRAYSIZE(expanded)) > 0) {
        outExe = expanded;
    }

    // If no directory slash, search in PATH
    if (outExe.find(L'\\') == std::wstring::npos && outExe.find(L'/') == std::wstring::npos) {
        wchar_t searched[MAX_PATH] = { 0 };
        if (SearchPathW(NULL, outExe.c_str(), L".exe", MAX_PATH, searched, NULL) > 0) {
            outExe = searched;
        }
    }

    // 2. Check if file exists on disk
    DWORD attr = GetFileAttributesW(outExe.c_str());
    outExists = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));

    // 3. Extract Publisher / Company Name
    outPublisher = "";
    if (outExists) {
        DWORD dummy = 0;
        DWORD verSize = GetFileVersionInfoSizeW(outExe.c_str(), &dummy);
        if (verSize > 0) {
            std::vector<BYTE> verData(verSize);
            if (GetFileVersionInfoW(outExe.c_str(), 0, verSize, verData.data())) {
                struct LANGANDCODEPAGE {
                    WORD wLanguage;
                    WORD wCodePage;
                } *lpTranslate = nullptr;
                UINT cbTranslate = 0;

                if (VerQueryValueW(verData.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate) &&
                    cbTranslate >= sizeof(LANGANDCODEPAGE)) {
                    wchar_t subBlock[128] = { 0 };
                    swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\CompanyName", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);
                    LPWSTR pCompany = nullptr;
                    UINT compLen = 0;
                    if (VerQueryValueW(verData.data(), subBlock, (LPVOID*)&pCompany, &compLen) && pCompany && compLen > 0) {
                        outPublisher = WideToUtf8(pCompany);
                    }
                }

                if (outPublisher.empty()) {
                    const wchar_t* fallbacks[] = {
                        L"\\StringFileInfo\\040904b0\\CompanyName",
                        L"\\StringFileInfo\\040904E4\\CompanyName",
                        L"\\StringFileInfo\\000004b0\\CompanyName",
                        L"\\StringFileInfo\\041904b0\\CompanyName",
                        L"\\StringFileInfo\\040904b0\\FileDescription"
                    };
                    for (const auto* fb : fallbacks) {
                        LPWSTR pCompany = nullptr;
                        UINT compLen = 0;
                        if (VerQueryValueW(verData.data(), fb, (LPVOID*)&pCompany, &compLen) && pCompany && compLen > 0) {
                            outPublisher = WideToUtf8(pCompany);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (outPublisher.empty()) {
        std::wstring lowerExe = outExe;
        for (auto& c : lowerExe) c = towlower(c);
        if (lowerExe.find(L"\\windows\\system32") != std::wstring::npos ||
            lowerExe.find(L"\\windows\\syswow64") != std::wstring::npos) {
            outPublisher = "Microsoft Corporation";
        } else {
            outPublisher = outExists ? "Не указан" : "Файл не найден";
        }
    }
}

static bool CheckApprovedState(HKEY hRoot, const std::wstring& approvedSubKey, const std::wstring& valueName, DWORD extraSam = 0) {
    // If querying HKLM, first check if user has an override in HKCU
    if (hRoot == HKEY_LOCAL_MACHINE) {
        ScopedHKey hUser;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, approvedSubKey.c_str(), 0, KEY_READ, hUser.put()) == ERROR_SUCCESS) {
            BYTE data[32] = { 0 };
            DWORD type = 0, size = sizeof(data);
            LONG res = RegQueryValueExW(hUser, valueName.c_str(), nullptr, &type, data, &size);
            if (res == ERROR_SUCCESS && size >= 1) {
                return (data[0] == 0x02);
            }
        }
    }

    ScopedHKey hKey;
    if (RegOpenKeyExW(hRoot, approvedSubKey.c_str(), 0, KEY_READ | extraSam, hKey.put()) == ERROR_SUCCESS) {
        BYTE data[32] = { 0 };
        DWORD type = 0, size = sizeof(data);
        LONG res = RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, data, &size);
        if (res == ERROR_SUCCESS && size >= 1) {
            return (data[0] == 0x02);
        }
    }

    // Default state in Windows is enabled if not found in StartupApproved
    return true;
}

static bool SetApprovedState(HKEY hRoot, const std::wstring& approvedSubKey, const std::wstring& valueName, bool enable, DWORD extraSam = 0) {
    ScopedHKey hKey;
    DWORD disposition = 0;
    if (RegCreateKeyExW(hRoot, approvedSubKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS | extraSam, nullptr, hKey.put(), &disposition) != ERROR_SUCCESS) {
        if (RegCreateKeyExW(hRoot, approvedSubKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE | extraSam, nullptr, hKey.put(), &disposition) != ERROR_SUCCESS) {
            return false;
        }
    }

    BYTE data[12] = { 0 };
    DWORD type = 0, size = sizeof(data);
    LONG res = RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type, data, &size);
    if (res != ERROR_SUCCESS || size < 12) {
        memset(data, 0, sizeof(data));
        size = 12;
    }

    data[0] = enable ? 0x02 : 0x03;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    memcpy(&data[4], &ft, sizeof(FILETIME));

    LONG setRes = RegSetValueExW(hKey, valueName.c_str(), 0, REG_BINARY, data, 12);
    hKey.Close();

    // If writing to HKLM, clean up any conflicting HKCU override
    if (hRoot == HKEY_LOCAL_MACHINE) {
        RegDeleteVal(HKEY_CURRENT_USER, approvedSubKey, valueName);
    }

    return (setRes == ERROR_SUCCESS);
}

// -----------------------------------------------------------------------------
// StartupManager Singleton Implementation
// -----------------------------------------------------------------------------

StartupManager& StartupManager::Get() {
    static StartupManager instance;
    return instance;
}

StartupManager::StartupManager() = default;

void StartupManager::Init() {
    ScanItems();
}

void StartupManager::ScanItems() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ScanItemsLocked();
}

void StartupManager::ScanRegistryKey(HKEY hRoot, const std::wstring& subKey, const std::wstring& approvedSubKey,
                                     StartupItem::LocationType locType, const std::string& locStr, DWORD extraSam) {
    ScopedHKey hKey;
    if (RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_READ | extraSam, hKey.put()) != ERROR_SUCCESS) {
        return;
    }

    DWORD index = 0;
    wchar_t valName[512] = { 0 };
    DWORD valNameLen = ARRAYSIZE(valName);
    DWORD type = 0;
    BYTE valData[4096] = { 0 };
    DWORD valDataLen = sizeof(valData);

    while (RegEnumValueW(hKey, index, valName, &valNameLen, nullptr, &type, valData, &valDataLen) == ERROR_SUCCESS) {
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            std::wstring cmd = (wchar_t*)valData;
            std::wstring rawName = valName;
            std::string utf8Name = WideToUtf8(rawName);

            StartupItem item;
            item.name = utf8Name.empty() ? "(По умолчанию)" : utf8Name;
            item.command = WideToUtf8(cmd);
            item.location = locStr;
            item.locType = locType;
            item.regValueName = rawName;

            std::string prefix;
            switch (locType) {
                case StartupItem::LocationType::HKCU_Run: prefix = "hkcu_run:"; break;
                case StartupItem::LocationType::HKLM_Run: prefix = "hklm_run:"; break;
                case StartupItem::LocationType::HKLM_Run32: prefix = "hklm32_run:"; break;
                default: prefix = "reg:"; break;
            }
            item.id = prefix + utf8Name;

            item.enabled = CheckApprovedState(hRoot, approvedSubKey, rawName, extraSam);

            std::wstring resolvedExe;
            ResolveExecutable(cmd, resolvedExe, item.publisher, item.exists);

            m_items.push_back(item);
        }

        index++;
        valNameLen = ARRAYSIZE(valName);
        valDataLen = sizeof(valData);
        memset(valName, 0, sizeof(valName));
        memset(valData, 0, sizeof(valData));
    }
}

void StartupManager::ScanFolder(const std::wstring& folderPath, StartupItem::LocationType locType, const std::string& locStr) {
    if (folderPath.empty() || GetFileAttributesW(folderPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    std::wstring pattern = folderPath + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        std::wstring filename = fd.cFileName;
        std::wstring lower = filename;
        for (auto& c : lower) c = towlower(c);

        if (lower == L"desktop.ini") continue;

        bool isDisabledExt = false;
        std::wstring baseFilename = filename;
        std::wstring baseLower = lower;

        if (lower.length() > 9 && lower.rfind(L".disabled") == lower.length() - 9) {
            isDisabledExt = true;
            baseFilename = filename.substr(0, filename.length() - 9);
            baseLower = lower.substr(0, lower.length() - 9);
        }

        // Verify supported executable / shortcut extensions
        bool validExt = false;
        const wchar_t* validExts[] = { L".lnk", L".exe", L".bat", L".cmd", L".url" };
        for (const auto* ve : validExts) {
            size_t veLen = wcslen(ve);
            if (baseLower.length() >= veLen && baseLower.rfind(ve) == baseLower.length() - veLen) {
                validExt = true;
                break;
            }
        }

        if (!validExt) continue;

        StartupItem item;
        item.locType = locType;
        item.location = locStr;
        item.filePath = folderPath + L"\\" + filename;

        // Friendly name (strip file extension)
        std::wstring displayName = baseFilename;
        size_t dotPos = displayName.find_last_of(L'.');
        if (dotPos != std::wstring::npos) {
            displayName = displayName.substr(0, dotPos);
        }
        item.name = WideToUtf8(displayName);

        std::string prefix = (locType == StartupItem::LocationType::Folder_User) ? "startup_user:" : "startup_common:";
        item.id = prefix + WideToUtf8(baseFilename);

        std::wstring targetExe;
        if (baseLower.length() >= 4 && baseLower.rfind(L".lnk") == baseLower.length() - 4) {
            std::wstring target, args;
            if (ResolveShortcut(item.filePath, target, args)) {
                item.command = WideToUtf8(target + (args.empty() ? L"" : L" " + args));
                targetExe = target;
            } else {
                item.command = WideToUtf8(item.filePath);
                targetExe = item.filePath;
            }
        } else {
            item.command = WideToUtf8(item.filePath);
            targetExe = item.filePath;
        }

        ResolveExecutable(targetExe, targetExe, item.publisher, item.exists);

        // Determine enabled status
        if (isDisabledExt) {
            item.enabled = false;
        } else {
            HKEY hRoot = (locType == StartupItem::LocationType::Folder_User) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
            item.enabled = CheckApprovedState(hRoot, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", baseFilename);
        }

        m_items.push_back(item);

    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void StartupManager::ScanItemsLocked() {
    m_items.clear();

    bool coInit = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        coInit = true;
    }

    // 1. HKCU Run
    ScanRegistryKey(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                    StartupItem::LocationType::HKCU_Run, "HKCU Run");

    // 2. HKLM Run
    ScanRegistryKey(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                    StartupItem::LocationType::HKLM_Run, "HKLM Run");

    // 3. HKLM (32-bit)
    ScanRegistryKey(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run",
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32",
                    StartupItem::LocationType::HKLM_Run32, "HKLM (32-bit)", KEY_WOW64_32KEY);

    // 4. User Startup folder: %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
    PWSTR pUserPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, NULL, &pUserPath)) && pUserPath) {
        ScanFolder(pUserPath, StartupItem::LocationType::Folder_User, "Startup Folder (User)");
        CoTaskMemFree(pUserPath);
    } else {
        wchar_t appdata[MAX_PATH] = { 0 };
        if (ExpandEnvironmentStringsW(L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", appdata, MAX_PATH) > 0) {
            ScanFolder(appdata, StartupItem::LocationType::Folder_User, "Startup Folder (User)");
        }
    }

    // 5. Common Startup folder: %PROGRAMDATA%\Microsoft\Windows\Start Menu\Programs\Startup
    PWSTR pCommonPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonStartup, 0, NULL, &pCommonPath)) && pCommonPath) {
        ScanFolder(pCommonPath, StartupItem::LocationType::Folder_Common, "Startup Folder (All Users)");
        CoTaskMemFree(pCommonPath);
    } else {
        wchar_t progdata[MAX_PATH] = { 0 };
        if (ExpandEnvironmentStringsW(L"%PROGRAMDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", progdata, MAX_PATH) > 0) {
            ScanFolder(progdata, StartupItem::LocationType::Folder_Common, "Startup Folder (All Users)");
        }
    }

    if (coInit) {
        CoUninitialize();
    }

    m_initialized = true;
}

nlohmann::json StartupManager::GetItemsJson() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        ScanItemsLocked();
    }

    nlohmann::json root;
    root["success"] = true;
    nlohmann::json itemsArr = nlohmann::json::array();
    int enabledCount = 0;

    for (const auto& item : m_items) {
        nlohmann::json j;
        j["id"] = item.id;
        j["name"] = item.name;
        j["command"] = item.command;
        j["location"] = item.location;
        j["enabled"] = item.enabled;
        j["publisher"] = item.publisher;
        j["exists"] = item.exists;
        itemsArr.push_back(j);

        if (item.enabled) {
            enabledCount++;
        }
    }

    root["items"] = itemsArr;
    root["totalCount"] = (int)m_items.size();
    root["enabledCount"] = enabledCount;
    return root;
}

nlohmann::json StartupManager::ToggleItem(const std::string& id, bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        ScanItemsLocked();
    }

    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const StartupItem& item) {
        return item.id == id;
    });

    if (it == m_items.end()) {
        nlohmann::json res;
        res["success"] = false;
        res["error"] = "Startup item not found";
        return res;
    }

    bool success = false;
    if (it->locType == StartupItem::LocationType::Folder_User || it->locType == StartupItem::LocationType::Folder_Common) {
        // Toggle folder item
        std::wstring curPath = it->filePath;
        std::wstring canonicalPath = curPath;
        if (canonicalPath.length() >= 9) {
            std::wstring ext = canonicalPath.substr(canonicalPath.length() - 9);
            for (auto& c : ext) c = towlower(c);
            if (ext == L".disabled") {
                canonicalPath = canonicalPath.substr(0, canonicalPath.length() - 9);
            }
        }

        std::wstring targetPath = enabled ? canonicalPath : (canonicalPath + L".disabled");
        if (curPath != targetPath) {
            if (MoveFileExW(curPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                it->filePath = targetPath;
                success = true;
            } else {
                success = false;
            }
        } else {
            success = true;
        }

        // Also update StartupApproved\StartupFolder
        size_t lastSlash = canonicalPath.find_last_of(L"\\/");
        std::wstring baseFileName = (lastSlash != std::wstring::npos) ? canonicalPath.substr(lastSlash + 1) : canonicalPath;
        HKEY hRoot = (it->locType == StartupItem::LocationType::Folder_User) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        SetApprovedState(hRoot, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", baseFileName, enabled);

        if (success) {
            it->enabled = enabled;
        }
    } else {
        // Registry item
        HKEY hRoot = (it->locType == StartupItem::LocationType::HKCU_Run) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        std::wstring approvedSubKey = (it->locType == StartupItem::LocationType::HKLM_Run32) ?
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32" :
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        DWORD extraSam = (it->locType == StartupItem::LocationType::HKLM_Run32) ? KEY_WOW64_32KEY : 0;

        success = SetApprovedState(hRoot, approvedSubKey, it->regValueName, enabled, extraSam);
        if (success) {
            it->enabled = enabled;
        }
    }

    nlohmann::json res;
    res["success"] = success;
    res["id"] = id;
    res["enabled"] = it->enabled;

    int enabledCount = 0;
    for (const auto& item : m_items) {
        if (item.enabled) enabledCount++;
    }
    res["totalCount"] = (int)m_items.size();
    res["enabledCount"] = enabledCount;
    return res;
}

nlohmann::json StartupManager::DeleteItem(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        ScanItemsLocked();
    }

    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const StartupItem& item) {
        return item.id == id;
    });

    if (it == m_items.end()) {
        nlohmann::json res;
        res["success"] = false;
        res["error"] = "Startup item not found";
        return res;
    }

    bool success = false;
    if (it->locType == StartupItem::LocationType::Folder_User || it->locType == StartupItem::LocationType::Folder_Common) {
        std::wstring curPath = it->filePath;
        std::wstring canonicalPath = curPath;
        if (canonicalPath.length() >= 9) {
            std::wstring ext = canonicalPath.substr(canonicalPath.length() - 9);
            for (auto& c : ext) c = towlower(c);
            if (ext == L".disabled") {
                canonicalPath = canonicalPath.substr(0, canonicalPath.length() - 9);
            }
        }

        DeleteFileW(curPath.c_str());
        DeleteFileW(canonicalPath.c_str());
        std::wstring disPath = canonicalPath + L".disabled";
        DeleteFileW(disPath.c_str());

        size_t lastSlash = canonicalPath.find_last_of(L"\\/");
        std::wstring baseFileName = (lastSlash != std::wstring::npos) ? canonicalPath.substr(lastSlash + 1) : canonicalPath;
        HKEY hRoot = (it->locType == StartupItem::LocationType::Folder_User) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        RegDeleteVal(hRoot, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", baseFileName);
        success = true;
    } else {
        HKEY hRoot = (it->locType == StartupItem::LocationType::HKCU_Run) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        std::wstring subKey = (it->locType == StartupItem::LocationType::HKLM_Run32) ?
            L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run" :
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        std::wstring approvedSubKey = (it->locType == StartupItem::LocationType::HKLM_Run32) ?
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32" :
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        DWORD extraSam = (it->locType == StartupItem::LocationType::HKLM_Run32) ? KEY_WOW64_32KEY : 0;

        RegDeleteVal(hRoot, subKey, it->regValueName, extraSam);
        RegDeleteVal(hRoot, approvedSubKey, it->regValueName, extraSam);

        if (hRoot == HKEY_LOCAL_MACHINE) {
            RegDeleteVal(HKEY_CURRENT_USER, approvedSubKey, it->regValueName);
        }
        success = true;
    }

    if (success) {
        m_items.erase(it);
    }

    nlohmann::json res;
    res["success"] = success;
    res["id"] = id;

    int enabledCount = 0;
    for (const auto& item : m_items) {
        if (item.enabled) enabledCount++;
    }
    res["totalCount"] = (int)m_items.size();
    res["enabledCount"] = enabledCount;
    return res;
}

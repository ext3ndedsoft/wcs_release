#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include "third_party/json.hpp"

struct Win11TweakItem {
    std::string id;
    std::string name;
    std::string description;
    bool enabled = false;
    bool isWin11Only = false;
};

class Win11Tweaker {
public:
    static Win11Tweaker& Get();

    void Init();
    void DetectState();

    nlohmann::json GetStateJson() const;
    bool HasTweak(const std::string& id) const;

    bool ToggleTweak(const std::string& id, bool enabled, std::string& outMsg);
    bool ToggleTweak(const std::string& id, bool enabled);

    bool IsWindows11() const { return m_isWindows11; }
    DWORD GetBuildNumber() const { return m_buildNumber; }

private:
    Win11Tweaker();
    ~Win11Tweaker() = default;

    void DetectOSVersion();
    void InitDatabase();

    mutable std::mutex m_mutex;
    bool m_isWindows11 = false;
    DWORD m_buildNumber = 0;
    DWORD m_majorVersion = 0;
    DWORD m_minorVersion = 0;
    std::vector<Win11TweakItem> m_items;

    // Registry helper methods
    static bool RegSetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD val);
    static bool RegDeleteVal(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName);
    static bool RegGetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD& outVal);
};

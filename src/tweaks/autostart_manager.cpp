#include "autostart_manager.h"
#include "config_manager.h"
#include "common_utils.h"
#include <vector>

AutostartManager& AutostartManager::Get() {
    static AutostartManager instance;
    return instance;
}

std::wstring AutostartManager::GetExecutablePath() const {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    return std::wstring(path);
}

bool AutostartManager::RunSilentCommand(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    bool ok = false;
    if (CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ScopedHandle hProc(pi.hProcess);
        ScopedHandle hThread(pi.hThread);
        WaitForSingleObject(hProc.Get(), 3000);
        DWORD exitCode = 1;
        if (GetExitCodeProcess(hProc.Get(), &exitCode)) {
            ok = (exitCode == 0);
        }
    }
    return ok;
}

bool AutostartManager::IsAutostartEnabled() {
    if (!m_initialized.load()) {
        m_cachedAutostart.store(ConfigManager::Get().GetConfig().autostartWithWindows);
    }
    return m_cachedAutostart.load();
}

bool AutostartManager::DisableAutostart() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // 1. Delete Task Scheduler tasks
    RunSilentCommand(L"schtasks.exe /delete /tn \"WindowsConfigStudio\" /f");
    RunSilentCommand(L"schtasks.exe /delete /tn \"Windows Esports Tweaker\" /f");

    // 2. Clean Run and RunOnce values from HKCU and HKLM via RegUtil
    static const std::wstring runKeys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"
    };
    static const std::wstring valNames[] = {
        L"WindowsConfigStudio",
        L"WindowsConfigStudio_Debug",
        L"Windows Esports Tweaker"
    };

    for (const auto& rk : runKeys) {
        for (const auto& vn : valNames) {
            RegUtil::DeleteVal(HKEY_CURRENT_USER, rk, vn);
            RegUtil::DeleteVal(HKEY_LOCAL_MACHINE, rk, vn);
        }
    }

    m_cachedAutostart.store(false);
    m_initialized.store(true);

    ConfigManager::Get().GetConfig().autostartWithWindows = false;
    ConfigManager::Get().Save();
    return true;
}

bool AutostartManager::SetAutostart(bool enable) {
    if (!enable) {
        return DisableAutostart();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    std::wstring exePath = GetExecutablePath();

    // Clean duplicate registry Run keys first
    RegUtil::DeleteVal(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"WindowsConfigStudio");

    m_cachedAutostart.store(true);
    m_initialized.store(true);

    // Create Task Scheduler task with highest available privileges so it launches elevated at logon without UAC prompts
    std::wstring schtasksCmd = L"schtasks.exe /create /tn \"WindowsConfigStudio\" /tr \"\\\"" + exePath + L"\\\" --autostart\" /sc onlogon /rl highest /f";
    bool schOk = RunSilentCommand(schtasksCmd);

    ConfigManager::Get().GetConfig().autostartWithWindows = true;
    ConfigManager::Get().Save();
    return schOk;
}

void AutostartManager::SyncStartupState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Always purge legacy duplicate registry Run key
    RegUtil::DeleteVal(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"WindowsConfigStudio");

    // Query once at application startup to populate cache
    std::wstring queryCmd = L"schtasks.exe /query /tn \"WindowsConfigStudio\"";
    bool scheduled = RunSilentCommand(queryCmd);
    m_cachedAutostart.store(scheduled);
    m_initialized.store(true);

    if (ConfigManager::Get().GetConfig().autostartWithWindows) {
        if (!scheduled) {
            std::wstring exePath = GetExecutablePath();
            std::wstring schtasksCmd = L"schtasks.exe /create /tn \"WindowsConfigStudio\" /tr \"\\\"" + exePath + L"\\\" --autostart\" /sc onlogon /rl highest /f";
            RunSilentCommand(schtasksCmd);
        }
    }
}

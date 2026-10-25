#include "debug_logger.h"
#include "common_utils.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <shlobj.h>

DebugLogger& DebugLogger::Get() {
    static DebugLogger instance;
    return instance;
}

DebugLogger::~DebugLogger() {
    if (m_logFile.is_open()) {
        m_logFile.close();
    }
}

void DebugLogger::Init(bool isDebugBuild) {
#if !defined(WCS_DEBUG_BUILD)
    (void)isDebugBuild;
    return; // Release build: do not create log files or allocate console
#else
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized) return;

        m_isDebug = isDebugBuild;
        if (!m_isDebug) return;

        wchar_t localAppData[MAX_PATH];
        std::filesystem::path logDir;
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
            logDir = std::filesystem::path(localAppData) / L"WindowsConfigStudio" / L"logs";
            std::error_code ec;
            std::filesystem::create_directories(logDir, ec);
        } else {
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(NULL, exePath, MAX_PATH);
            logDir = std::filesystem::path(exePath).parent_path();
        }
        std::string logFilePath = (logDir / "wcs_debug.log").string();

        m_logFile.open(logFilePath, std::ios::out | std::ios::trunc);

        m_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        if (m_hConsole == NULL || m_hConsole == INVALID_HANDLE_VALUE) {
            AllocConsole();
            m_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        }
        if (m_hConsole != NULL && m_hConsole != INVALID_HANDLE_VALUE) {
            SetConsoleTitleW(L"Windows Config Studio [DEBUG CONSOLE] — Live Tweak & System Diagnostics");
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
        }

        m_initialized = true;
    } catch (...) {}

    Log(LogLevel::Info, "SYSTEM", "==================================================================");
    Log(LogLevel::Info, "SYSTEM", "Windows Config Studio // Debug Diagnostic Console Initialized");
    Log(LogLevel::Info, "SYSTEM", "==================================================================");
#endif
}

std::string DebugLogger::GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);

    std::tm bt;
    localtime_s(&bt, &timer);

    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void DebugLogger::PrintColor(WORD color, const std::string& text) {
    if (m_hConsole != NULL && m_hConsole != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        WORD oldColor = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        if (GetConsoleScreenBufferInfo(m_hConsole, &csbi)) {
            oldColor = csbi.wAttributes;
        }
        SetConsoleTextAttribute(m_hConsole, color);
        DWORD written = 0;
        WriteConsoleA(m_hConsole, text.data(), static_cast<DWORD>(text.size()), &written, NULL);
        SetConsoleTextAttribute(m_hConsole, oldColor);
    }
}

void DebugLogger::Log(LogLevel level, const std::string& tag, const std::string& message) {
#if !defined(WCS_DEBUG_BUILD)
    (void)level; (void)tag; (void)message;
    return;
#else
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string timeStr = GetTimestamp();
    std::string levelStr;
    WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

    switch (level) {
        case LogLevel::Info:
            levelStr = "[INFO]   ";
            color = FOREGROUND_INTENSITY | FOREGROUND_BLUE | FOREGROUND_GREEN; // Cyan
            break;
        case LogLevel::Success:
            levelStr = "[SUCCESS]";
            color = FOREGROUND_INTENSITY | FOREGROUND_GREEN; // Bright Green
            break;
        case LogLevel::Warning:
            levelStr = "[WARN]   ";
            color = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN; // Yellow
            break;
        case LogLevel::Error:
            levelStr = "[ERROR]  ";
            color = FOREGROUND_INTENSITY | FOREGROUND_RED; // Bright Red
            break;
        case LogLevel::Tweak:
            levelStr = "[TWEAK]  ";
            color = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE; // Magenta / Purple
            break;
        case LogLevel::Registry:
            levelStr = "[REG]    ";
            color = FOREGROUND_INTENSITY | FOREGROUND_BLUE; // Blue
            break;
        case LogLevel::Service:
            levelStr = "[SERVICE]";
            color = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            break;
    }

    std::string fullMsg = "[" + timeStr + "] " + levelStr + " [" + tag + "] " + message + "\n";

    if (m_logFile.is_open()) {
        m_logFile << fullMsg;
        m_logFile.flush();
    }

    if (m_isDebug) {
        PrintColor(FOREGROUND_INTENSITY | FOREGROUND_BLUE, "[" + timeStr + "] ");
        PrintColor(color, levelStr);
        PrintColor(FOREGROUND_INTENSITY, " [" + tag + "] ");
        PrintColor(color, message + "\n");
    }
#endif
}

void DebugLogger::LogRegistry(const std::wstring& root, const std::wstring& path, const std::wstring& value, DWORD data, bool success) {
#if !defined(WCS_DEBUG_BUILD)
    (void)root; (void)path; (void)value; (void)data; (void)success;
#else
    std::string r = WideStrToUtf8(root);
    std::string p = WideStrToUtf8(path);
    std::string v = WideStrToUtf8(value);
    std::string msg = r + "\\" + p + " -> [" + v + "] = " + std::to_string(data) + " " + (success ? "(OK)" : "(FAILED)");
    Log(success ? LogLevel::Registry : LogLevel::Error, "REGISTRY", msg);
#endif
}

void DebugLogger::LogService(const std::wstring& svcName, DWORD targetStart, bool success) {
#if !defined(WCS_DEBUG_BUILD)
    (void)svcName; (void)targetStart; (void)success;
#else
    std::string s = WideStrToUtf8(svcName);
    std::string st = (targetStart == SERVICE_DISABLED) ? "DISABLED" : (targetStart == SERVICE_AUTO_START ? "AUTO" : "MANUAL");
    std::string msg = "Service [" + s + "] -> " + st + " " + (success ? "(OK)" : "(FAILED)");
    Log(success ? LogLevel::Service : LogLevel::Error, "SERVICE", msg);
#endif
}

void DebugLogger::LogTweak(const std::string& tweakName, bool enabled, const std::string& details) {
#if !defined(WCS_DEBUG_BUILD)
    (void)tweakName; (void)enabled; (void)details;
#else
    std::string msg = tweakName + " = " + (enabled ? "ON" : "OFF") + " (" + details + ")";
    Log(LogLevel::Tweak, "TWEAK_STATE", msg);
#endif
}

void DebugLogger::LogTimer(float targetMs, float currentMs, bool success) {
#if !defined(WCS_DEBUG_BUILD)
    (void)targetMs; (void)currentMs; (void)success;
#else
    std::string msg = "Target: " + std::to_string(targetMs) + "ms | Current live resolution: " + std::to_string(currentMs) + "ms | Lock: " + (success ? "ACTIVE" : "STOPPED");
    Log(success ? LogLevel::Success : LogLevel::Info, "TIMER_RES", msg);
#endif
}

#pragma once

#include <windows.h>
#include <string>
#include <fstream>
#include <mutex>
#include <iostream>

enum class LogLevel {
    Info,
    Success,
    Warning,
    Error,
    Tweak,
    Registry,
    Service
};

class DebugLogger {
public:
    static DebugLogger& Get();

    void Init(bool isDebugBuild);
    void Log(LogLevel level, const std::string& tag, const std::string& message);

    void LogRegistry(const std::wstring& root, const std::wstring& path, const std::wstring& value, DWORD data, bool success);
    void LogService(const std::wstring& svcName, DWORD targetStart, bool success);
    void LogTweak(const std::string& tweakName, bool enabled, const std::string& details);
    void LogTimer(float targetMs, float currentMs, bool success);

private:
    DebugLogger() = default;
    ~DebugLogger();

    bool m_initialized = false;
    bool m_isDebug = false;
    std::ofstream m_logFile;
    std::mutex m_mutex;
    HANDLE m_hConsole = INVALID_HANDLE_VALUE;

    void PrintColor(WORD color, const std::string& text);
    std::string GetTimestamp();
};

#if defined(WCS_DEBUG_BUILD)
#define DEBUG_LOG_INFO(tag, msg) DebugLogger::Get().Log(LogLevel::Info, tag, msg)
#define DEBUG_LOG_SUCCESS(tag, msg) DebugLogger::Get().Log(LogLevel::Success, tag, msg)
#define DEBUG_LOG_WARN(tag, msg) DebugLogger::Get().Log(LogLevel::Warning, tag, msg)
#define DEBUG_LOG_ERROR(tag, msg) DebugLogger::Get().Log(LogLevel::Error, tag, msg)
#else
#define DEBUG_LOG_INFO(tag, msg) ((void)0)
#define DEBUG_LOG_SUCCESS(tag, msg) ((void)0)
#define DEBUG_LOG_WARN(tag, msg) ((void)0)
#define DEBUG_LOG_ERROR(tag, msg) ((void)0)
#endif

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <setupapi.h>
#include <wininet.h>
#include <winsvc.h>
#include <memory>
#include <algorithm>

// ============================================================================
// RAII Scope Guards for Win32 Handles & Resources
// ============================================================================

// Scoped HKEY wrapper ensuring RegCloseKey is called unless it's a predefined root key
class ScopedHKey {
public:
    explicit ScopedHKey(HKEY h = nullptr) noexcept : m_hKey(h) {}
    ~ScopedHKey() noexcept { Close(); }

    ScopedHKey(const ScopedHKey&) = delete;
    ScopedHKey& operator=(const ScopedHKey&) = delete;

    ScopedHKey(ScopedHKey&& other) noexcept : m_hKey(other.m_hKey) {
        other.m_hKey = nullptr;
    }

    ScopedHKey& operator=(ScopedHKey&& other) noexcept {
        if (this != &other) {
            Close();
            m_hKey = other.m_hKey;
            other.m_hKey = nullptr;
        }
        return *this;
    }

    HKEY get() const noexcept { return m_hKey; }
    HKEY Get() const noexcept { return m_hKey; }
    operator HKEY() const noexcept { return m_hKey; }
    HKEY* put() noexcept { Close(); return &m_hKey; }
    HKEY* GetAddressOf() noexcept { return put(); }
    void reset(HKEY h = nullptr) noexcept { Close(); m_hKey = h; }
    void Reset(HKEY h = nullptr) noexcept { reset(h); }
    HKEY release() noexcept { HKEY tmp = m_hKey; m_hKey = nullptr; return tmp; }

    bool isValid() const noexcept {
        return m_hKey != nullptr && m_hKey != INVALID_HANDLE_VALUE;
    }
    bool IsValid() const noexcept { return isValid(); }
    explicit operator bool() const noexcept { return isValid(); }

    void Close() noexcept {
        if (m_hKey && m_hKey != INVALID_HANDLE_VALUE &&
            m_hKey != HKEY_CLASSES_ROOT && m_hKey != HKEY_CURRENT_USER &&
            m_hKey != HKEY_LOCAL_MACHINE && m_hKey != HKEY_USERS &&
            m_hKey != HKEY_CURRENT_CONFIG && m_hKey != HKEY_PERFORMANCE_DATA) {
            ::RegCloseKey(m_hKey);
            m_hKey = nullptr;
        }
    }

private:
    HKEY m_hKey = nullptr;
};

// Scoped Win32 HANDLE wrapper ensuring CloseHandle is called
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : m_handle(h) {}
    ~ScopedHandle() noexcept { Close(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = INVALID_HANDLE_VALUE;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            Close();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const noexcept { return m_handle; }
    HANDLE Get() const noexcept { return m_handle; }
    operator HANDLE() const noexcept { return m_handle; }
    HANDLE* put() noexcept { Close(); return &m_handle; }
    HANDLE* GetAddressOf() noexcept { return put(); }
    void reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept { Close(); m_handle = h; }
    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept { reset(h); }
    HANDLE release() noexcept { HANDLE tmp = m_handle; m_handle = INVALID_HANDLE_VALUE; return tmp; }

    bool isValid() const noexcept {
        return m_handle != NULL && m_handle != INVALID_HANDLE_VALUE;
    }
    bool IsValid() const noexcept { return isValid(); }
    explicit operator bool() const noexcept { return isValid(); }

    void Close() noexcept {
        if (m_handle != NULL && m_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

// Scoped HINTERNET wrapper ensuring InternetCloseHandle is called
class ScopedHInternet {
public:
    explicit ScopedHInternet(HINTERNET h = NULL) noexcept : m_hInternet(h) {}
    ~ScopedHInternet() noexcept { Close(); }

    ScopedHInternet(const ScopedHInternet&) = delete;
    ScopedHInternet& operator=(const ScopedHInternet&) = delete;

    ScopedHInternet(ScopedHInternet&& other) noexcept : m_hInternet(other.m_hInternet) {
        other.m_hInternet = NULL;
    }

    ScopedHInternet& operator=(ScopedHInternet&& other) noexcept {
        if (this != &other) {
            Close();
            m_hInternet = other.m_hInternet;
            other.m_hInternet = NULL;
        }
        return *this;
    }

    HINTERNET get() const noexcept { return m_hInternet; }
    HINTERNET Get() const noexcept { return m_hInternet; }
    operator HINTERNET() const noexcept { return m_hInternet; }
    HINTERNET* put() noexcept { Close(); return &m_hInternet; }
    HINTERNET* GetAddressOf() noexcept { return put(); }
    void reset(HINTERNET h = NULL) noexcept { Close(); m_hInternet = h; }
    void Reset(HINTERNET h = NULL) noexcept { reset(h); }
    HINTERNET release() noexcept { HINTERNET tmp = m_hInternet; m_hInternet = NULL; return tmp; }

    bool isValid() const noexcept { return m_hInternet != NULL; }
    bool IsValid() const noexcept { return isValid(); }
    explicit operator bool() const noexcept { return isValid(); }

    void Close() noexcept {
        if (m_hInternet != NULL) {
            ::InternetCloseHandle(m_hInternet);
            m_hInternet = NULL;
        }
    }

private:
    HINTERNET m_hInternet = NULL;
};

// Scoped HDEVINFO wrapper ensuring SetupDiDestroyDeviceInfoList is called
class ScopedHDevInfo {
public:
    explicit ScopedHDevInfo(HDEVINFO h = INVALID_HANDLE_VALUE) noexcept : m_hDevInfo(h) {}
    ~ScopedHDevInfo() noexcept { Close(); }

    ScopedHDevInfo(const ScopedHDevInfo&) = delete;
    ScopedHDevInfo& operator=(const ScopedHDevInfo&) = delete;

    ScopedHDevInfo(ScopedHDevInfo&& other) noexcept : m_hDevInfo(other.m_hDevInfo) {
        other.m_hDevInfo = INVALID_HANDLE_VALUE;
    }

    ScopedHDevInfo& operator=(ScopedHDevInfo&& other) noexcept {
        if (this != &other) {
            Close();
            m_hDevInfo = other.m_hDevInfo;
            other.m_hDevInfo = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HDEVINFO get() const noexcept { return m_hDevInfo; }
    HDEVINFO Get() const noexcept { return m_hDevInfo; }
    operator HDEVINFO() const noexcept { return m_hDevInfo; }
    HDEVINFO* put() noexcept { Close(); return &m_hDevInfo; }
    HDEVINFO* GetAddressOf() noexcept { return put(); }
    void reset(HDEVINFO h = INVALID_HANDLE_VALUE) noexcept { Close(); m_hDevInfo = h; }
    void Reset(HDEVINFO h = INVALID_HANDLE_VALUE) noexcept { reset(h); }
    HDEVINFO release() noexcept { HDEVINFO tmp = m_hDevInfo; m_hDevInfo = INVALID_HANDLE_VALUE; return tmp; }

    bool isValid() const noexcept {
        return m_hDevInfo != INVALID_HANDLE_VALUE && m_hDevInfo != NULL;
    }
    bool IsValid() const noexcept { return isValid(); }
    explicit operator bool() const noexcept { return isValid(); }

    void Close() noexcept {
        if (m_hDevInfo != INVALID_HANDLE_VALUE && m_hDevInfo != NULL) {
            ::SetupDiDestroyDeviceInfoList(m_hDevInfo);
            m_hDevInfo = INVALID_HANDLE_VALUE;
        }
    }

private:
    HDEVINFO m_hDevInfo = INVALID_HANDLE_VALUE;
};

// Scoped SC_HANDLE wrapper ensuring CloseServiceHandle is called
class ScopedScHandle {
public:
    explicit ScopedScHandle(SC_HANDLE h = NULL) noexcept : m_handle(h) {}
    ~ScopedScHandle() noexcept { Close(); }

    ScopedScHandle(const ScopedScHandle&) = delete;
    ScopedScHandle& operator=(const ScopedScHandle&) = delete;

    ScopedScHandle(ScopedScHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = NULL;
    }

    ScopedScHandle& operator=(ScopedScHandle&& other) noexcept {
        if (this != &other) {
            Close();
            m_handle = other.m_handle;
            other.m_handle = NULL;
        }
        return *this;
    }

    SC_HANDLE get() const noexcept { return m_handle; }
    SC_HANDLE Get() const noexcept { return m_handle; }
    operator SC_HANDLE() const noexcept { return m_handle; }
    SC_HANDLE* put() noexcept { Close(); return &m_handle; }
    SC_HANDLE* GetAddressOf() noexcept { return put(); }
    void reset(SC_HANDLE h = NULL) noexcept { Close(); m_handle = h; }
    void Reset(SC_HANDLE h = NULL) noexcept { reset(h); }
    SC_HANDLE release() noexcept { SC_HANDLE tmp = m_handle; m_handle = NULL; return tmp; }

    bool isValid() const noexcept { return m_handle != NULL; }
    bool IsValid() const noexcept { return isValid(); }
    explicit operator bool() const noexcept { return isValid(); }

    void Close() noexcept {
        if (m_handle != NULL) {
            ::CloseServiceHandle(m_handle);
            m_handle = NULL;
        }
    }

private:
    SC_HANDLE m_handle = NULL;
};

// ============================================================================
// String Conversion Utilities
// ============================================================================

std::wstring Utf8ToWideStr(const std::string& str);
inline std::wstring Utf8ToWide(const std::string& str) { return Utf8ToWideStr(str); }

std::string WideStrToUtf8(const std::wstring& wstr);
inline std::string WideToUtf8(const std::wstring& wstr) { return WideStrToUtf8(wstr); }

std::string ToLower(const std::string& str);
std::string Trim(const std::string& str);

// ============================================================================
// Unified Registry Helpers
// ============================================================================

namespace RegUtil {

bool SetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD val, DWORD extraSam = 0);
bool GetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD& outVal, DWORD extraSam = 0);

bool SetQword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, unsigned __int64 val, DWORD extraSam = 0);
bool GetQword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, unsigned __int64& outVal, DWORD extraSam = 0);

bool SetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, const std::wstring& val, DWORD extraSam = 0);
bool GetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, std::wstring& outVal, DWORD extraSam = 0);

bool SetBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, const BYTE* data, DWORD size, DWORD extraSam = 0);
bool GetBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, BYTE* outData, DWORD& inOutSize, DWORD extraSam = 0);

bool DeleteVal(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD extraSam = 0);
bool DeleteSubKeyTree(HKEY hRoot, const std::wstring& subKey);

} // namespace RegUtil

// Common inline forwarded aliases for backwards compatibility
inline bool RegSetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD val, DWORD extraSam = 0) {
    return RegUtil::SetDword(hRoot, subKey, valName, val, extraSam);
}
inline bool RegGetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD& outVal, DWORD extraSam = 0) {
    return RegUtil::GetDword(hRoot, subKey, valName, outVal, extraSam);
}
inline bool RegSetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, const std::wstring& val, DWORD extraSam = 0) {
    return RegUtil::SetString(hRoot, subKey, valName, val, extraSam);
}
inline bool RegGetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, std::wstring& outVal, DWORD extraSam = 0) {
    return RegUtil::GetString(hRoot, subKey, valName, outVal, extraSam);
}
inline bool RegDeleteVal(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD extraSam = 0) {
    return RegUtil::DeleteVal(hRoot, subKey, valName, extraSam);
}

#include "common_utils.h"

// ============================================================================
// String Conversion Utilities
// ============================================================================

std::wstring Utf8ToWideStr(const std::string& str) {
    if (str.empty()) return std::wstring();
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), nullptr, 0);
    if (sizeNeeded <= 0) return std::wstring();
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), &wstr[0], sizeNeeded);
    return wstr;
}

std::string WideStrToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return std::string();
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

std::string ToLower(const std::string& str) {
    std::string res = str;
    std::transform(res.begin(), res.end(), res.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return res;
}

std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// ============================================================================
// Unified Registry Helpers
// ============================================================================

namespace RegUtil {

bool SetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD val, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_SET_VALUE | extraSam, hKey.put());
    if (status != ERROR_SUCCESS) {
        status = RegCreateKeyExW(hRoot, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_CREATE_SUB_KEY | extraSam, nullptr, hKey.put(), nullptr);
    }
    if (status != ERROR_SUCCESS) return false;
    status = RegSetValueExW(hKey, valName.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
    return (status == ERROR_SUCCESS);
}

bool GetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD& outVal, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_QUERY_VALUE | extraSam, hKey.put());
    if (status != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    status = RegQueryValueExW(hKey, valName.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(&outVal), &size);
    return (status == ERROR_SUCCESS && (type == REG_DWORD || type == REG_QWORD));
}

bool SetQword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, unsigned __int64 val, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegCreateKeyExW(hRoot, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | extraSam, nullptr, hKey.put(), nullptr);
    if (status != ERROR_SUCCESS) return false;
    status = RegSetValueExW(hKey, valName.c_str(), 0, REG_QWORD, reinterpret_cast<const BYTE*>(&val), sizeof(unsigned __int64));
    return (status == ERROR_SUCCESS);
}

bool GetQword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, unsigned __int64& outVal, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_READ | extraSam, hKey.put());
    if (status != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = sizeof(unsigned __int64);
    status = RegQueryValueExW(hKey, valName.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(&outVal), &size);
    return (status == ERROR_SUCCESS && (type == REG_QWORD || type == REG_DWORD));
}

bool SetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, const std::wstring& val, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegCreateKeyExW(hRoot, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | extraSam, nullptr, hKey.put(), nullptr);
    if (status != ERROR_SUCCESS) return false;
    DWORD byteLen = static_cast<DWORD>((val.length() + 1) * sizeof(wchar_t));
    status = RegSetValueExW(hKey, valName.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), byteLen);
    return (status == ERROR_SUCCESS);
}

bool GetString(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, std::wstring& outVal, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_READ | extraSam, hKey.put());
    if (status != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = 0;
    status = RegQueryValueExW(hKey, valName.c_str(), nullptr, &type, nullptr, &size);
    if (status != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) return false;
    if (size == 0) {
        outVal.clear();
        return true;
    }
    std::vector<wchar_t> buf((size / sizeof(wchar_t)) + 2, 0);
    status = RegQueryValueExW(hKey, valName.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size);
    if (status == ERROR_SUCCESS) {
        outVal = buf.data();
        return true;
    }
    return false;
}

bool SetBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, const BYTE* data, DWORD size, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegCreateKeyExW(hRoot, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE | extraSam, nullptr, hKey.put(), nullptr);
    if (status != ERROR_SUCCESS) return false;
    status = RegSetValueExW(hKey, valName.c_str(), 0, REG_BINARY, data, size);
    return (status == ERROR_SUCCESS);
}

bool GetBinary(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, BYTE* outData, DWORD& inOutSize, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_READ | extraSam, hKey.put());
    if (status != ERROR_SUCCESS) return false;
    DWORD type = 0;
    status = RegQueryValueExW(hKey, valName.c_str(), nullptr, &type, outData, &inOutSize);
    return (status == ERROR_SUCCESS && (type == REG_BINARY || type == REG_NONE));
}

bool DeleteVal(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD extraSam) {
    ScopedHKey hKey;
    LSTATUS status = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_SET_VALUE | extraSam, hKey.put());
    if (status != ERROR_SUCCESS) return false;
    status = RegDeleteValueW(hKey, valName.c_str());
    return (status == ERROR_SUCCESS);
}

bool DeleteSubKeyTree(HKEY hRoot, const std::wstring& subKey) {
    LSTATUS status = RegDeleteTreeW(hRoot, subKey.c_str());
    return (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND);
}

} // namespace RegUtil

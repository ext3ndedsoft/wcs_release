#include "resource_bundle.h"
#include "resource.h"
#include "debug_logger.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <vector>
#include <cstdint>

static std::filesystem::path GetLocalAppDataResourcesPath() {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        std::filesystem::path p(localAppData);
        p /= L"WindowsConfigStudio";
        p /= L"resources";
        return p;
    }
    return L"";
}

bool ResourceBundle::ExtractBundleTo(const std::filesystem::path& targetDir) {
    HMODULE hModule = GetModuleHandleW(NULL);
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_RESOURCES_BUNDLE), (LPCWSTR)RT_RCDATA);
    if (!hRes) {
        DEBUG_LOG_WARN("RES_BUNDLE", "FindResourceW failed for IDR_RESOURCES_BUNDLE");
        return false;
    }

    HGLOBAL hMem = LoadResource(hModule, hRes);
    if (!hMem) {
        DEBUG_LOG_ERROR("RES_BUNDLE", "LoadResource failed");
        return false;
    }

    DWORD size = SizeofResource(hModule, hRes);
    const uint8_t* data = (const uint8_t*)LockResource(hMem);
    if (!data || size < 8) {
        DEBUG_LOG_ERROR("RES_BUNDLE", "LockResource failed or invalid size");
        return false;
    }

    // Check magic "WCSP"
    if (data[0] != 'W' || data[1] != 'C' || data[2] != 'S' || data[3] != 'P') {
        DEBUG_LOG_ERROR("RES_BUNDLE", "Invalid bundle magic");
        return false;
    }

    uint32_t fileCount = *(const uint32_t*)(data + 4);
    size_t offset = 8;

    std::filesystem::create_directories(targetDir);

    for (uint32_t i = 0; i < fileCount; i++) {
        if (offset + 2 > size) break;
        uint16_t pathLen = *(const uint16_t*)(data + offset);
        offset += 2;

        if (offset + pathLen > size) break;
        std::string relPath((const char*)(data + offset), pathLen);
        offset += pathLen;

        if (offset + 4 > size) break;
        uint32_t contentLen = *(const uint32_t*)(data + offset);
        offset += 4;

        if (offset + contentLen > size) break;
        const uint8_t* content = data + offset;
        offset += contentLen;

        std::filesystem::path destPath = targetDir / relPath;
        std::filesystem::create_directories(destPath.parent_path());

        bool needWrite = true;
        if (std::filesystem::exists(destPath)) {
            std::error_code ec;
            if (std::filesystem::file_size(destPath, ec) == contentLen) {
                needWrite = false;
            }
        }

        if (needWrite) {
            std::ofstream out(destPath, std::ios::binary);
            if (out.is_open()) {
                out.write((const char*)content, contentLen);
            }
        }
    }

    DEBUG_LOG_INFO("RES_BUNDLE", "Successfully verified/extracted " + std::to_string(fileCount) + " embedded resource files.");
    return true;
}

bool ResourceBundle::EnsureResourcesReady() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    std::filesystem::path rootDir = p.parent_path();

    // 1. If local resources/web/index.html exists next to exe, use it directly (developer mode)
    if (std::filesystem::exists(rootDir / "resources" / "web" / "index.html")) {
        return true;
    }

    // 2. Otherwise ensure %LOCALAPPDATA%\WindowsConfigStudio\resources is extracted from embedded bundle
    std::filesystem::path appDataRes = GetLocalAppDataResourcesPath();
    if (!appDataRes.empty()) {
        ExtractBundleTo(appDataRes);
        if (std::filesystem::exists(appDataRes / "web" / "index.html")) {
            return true;
        }
    }

    return false;
}

std::wstring ResourceBundle::GetResourcesDirectory() {
    EnsureResourcesReady();

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    std::filesystem::path rootDir = p.parent_path();

    // Priority 1: Next to exe
    if (std::filesystem::exists(rootDir / "resources" / "web" / "index.html")) {
        return std::filesystem::canonical(rootDir / "resources").wstring();
    }
    // Priority 2: Bin parent (development mode)
    if (std::filesystem::exists(rootDir.parent_path() / "resources" / "web" / "index.html")) {
        return std::filesystem::canonical(rootDir.parent_path() / "resources").wstring();
    }
    // Priority 3: LocalAppData extracted bundle
    std::filesystem::path appDataRes = GetLocalAppDataResourcesPath();
    if (std::filesystem::exists(appDataRes / "web" / "index.html")) {
        return std::filesystem::canonical(appDataRes).wstring();
    }

    // Priority 4: Documents workspace
    wchar_t myDocs[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYDOCUMENTS, NULL, 0, myDocs))) {
        std::vector<std::filesystem::path> docCandidates = {
            std::filesystem::path(myDocs) / "Windows Esports Tweaker WebView" / "bin" / "resources",
            std::filesystem::path(myDocs) / "Windows Esports Tweaker WebView" / "resources"
        };
        for (const auto& cand : docCandidates) {
            if (std::filesystem::exists(cand / "web" / "index.html")) {
                return std::filesystem::canonical(cand).wstring();
            }
        }
    }

    return (rootDir / "resources").wstring();
}

#pragma once

#include <string>
#include <filesystem>

class ResourceBundle {
public:
    // Ensures resources are available: extracts embedded bundle to %LOCALAPPDATA%\WindowsConfigStudio\resources if needed
    static bool EnsureResourcesReady();

    // Returns the canonical path to the directory containing web/index.html
    static std::wstring GetResourcesDirectory();

    // Unpacks embedded bundle into target directory
    static bool ExtractBundleTo(const std::filesystem::path& targetDir);
};

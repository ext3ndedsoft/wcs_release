#include "msi_manager.h"
#include "common_utils.h"
#include <algorithm>
#include <cwctype>
#include <setupapi.h>
#include <cfgmgr32.h>

static std::wstring ReadRegString(HKEY hKey, const wchar_t* valName) {
    DWORD type = 0;
    DWORD cbData = 0;
    if (RegQueryValueExW(hKey, valName, NULL, &type, NULL, &cbData) != ERROR_SUCCESS || cbData == 0) {
        return L"";
    }
    std::vector<wchar_t> buf(cbData / sizeof(wchar_t) + 2, 0);
    if (RegQueryValueExW(hKey, valName, NULL, &type, (LPBYTE)buf.data(), &cbData) != ERROR_SUCCESS) {
        return L"";
    }
    return std::wstring(buf.data());
}

static bool ReadRegDword(HKEY hKey, const wchar_t* valName, DWORD& outVal) {
    DWORD type = 0;
    DWORD val = 0;
    DWORD cbData = sizeof(val);
    if (RegQueryValueExW(hKey, valName, NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS && type == REG_DWORD) {
        outVal = val;
        return true;
    }
    return false;
}

static std::wstring ExtractCleanName(const std::wstring& rawDesc, const std::wstring& rawFriendly) {
    auto parseDesc = [](const std::wstring& s) -> std::wstring {
        if (s.empty()) return L"";
        if (s[0] != L'@') {
            return s;
        }
        size_t firstSemi = s.find(L';');
        if (firstSemi == std::wstring::npos) {
            return s;
        }
        std::wstring afterFirst = s.substr(firstSemi + 1);
        size_t secondSemi = afterFirst.find(L';');
        if (secondSemi == std::wstring::npos) {
            return afterFirst;
        }

        std::wstring text = afterFirst.substr(0, secondSemi);
        std::wstring params = afterFirst.substr(secondSemi + 1);
        if (params.size() >= 2 && params.front() == L'(' && params.back() == L')') {
            params = params.substr(1, params.size() - 2);
        }

        std::vector<std::wstring> args;
        size_t start = 0;
        while (start < params.size()) {
            size_t comma = params.find(L',', start);
            if (comma == std::wstring::npos) {
                args.push_back(params.substr(start));
                break;
            }
            args.push_back(params.substr(start, comma - start));
            start = comma + 1;
        }

        for (size_t i = 0; i < args.size(); ++i) {
            std::wstring ph = L"%" + std::to_wstring(i + 1);
            size_t pos = 0;
            while ((pos = text.find(ph, pos)) != std::wstring::npos) {
                text.replace(pos, ph.length(), args[i]);
                pos += args[i].length();
            }
        }
        return text;
    };

    std::wstring fn = parseDesc(rawFriendly);
    std::wstring dd = parseDesc(rawDesc);

    if (!fn.empty() && fn.find(L'%') == std::wstring::npos) {
        return fn;
    }
    if (!dd.empty()) {
        return dd;
    }
    return fn.empty() ? rawDesc : fn;
}

static std::wstring CleanLocationInfo(const std::wstring& rawLoc) {
    if (rawLoc.empty()) return L"PCI Device";
    if (rawLoc[0] != L'@') return rawLoc;

    size_t lastOpen = rawLoc.rfind(L'(');
    size_t lastClose = rawLoc.rfind(L')');
    if (lastOpen != std::wstring::npos && lastClose != std::wstring::npos && lastClose > lastOpen) {
        std::wstring inner = rawLoc.substr(lastOpen + 1, lastClose - lastOpen - 1);
        size_t c1 = inner.find(L',');
        size_t c2 = (c1 != std::wstring::npos) ? inner.find(L',', c1 + 1) : std::wstring::npos;
        if (c1 != std::wstring::npos && c2 != std::wstring::npos) {
            std::wstring bus = inner.substr(0, c1);
            std::wstring dev = inner.substr(c1 + 1, c2 - c1 - 1);
            std::wstring fn = inner.substr(c2 + 1);
            return L"Bus " + bus + L", Dev " + dev + L", Func " + fn;
        }
        return L"PCI (" + inner + L")";
    }
    return rawLoc;
}

static std::string ClassifyDevice(const std::wstring& classStr, const std::wstring& classGuidStr,
                                  const std::wstring& name, const std::wstring& hwId) {
    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };
    std::wstring c = toLower(classStr);
    std::wstring g = toLower(classGuidStr);
    std::wstring n = toLower(name);
    std::wstring h = toLower(hwId);

    // 1. Display / GPU
    if (g == L"{4d36e968-e325-11ce-bfc1-08002be10318}" || c == L"display" ||
        h.find(L"&cc_03") != std::wstring::npos ||
        n.find(L"geforce") != std::wstring::npos || n.find(L"radeon") != std::wstring::npos ||
        n.find(L"rtx ") != std::wstring::npos || n.find(L"gtx ") != std::wstring::npos ||
        n.find(L"graphics") != std::wstring::npos || n.find(L"arc(tm)") != std::wstring::npos) {
        return "GPU";
    }

    // 2. Network (Ethernet, Wi-Fi)
    if (g == L"{4d36e972-e325-11ce-bfc1-08002be10318}" || c == L"net" ||
        h.find(L"&cc_02") != std::wstring::npos ||
        n.find(L"ethernet") != std::wstring::npos || n.find(L"wi-fi") != std::wstring::npos ||
        n.find(L"wireless") != std::wstring::npos || n.find(L"network") != std::wstring::npos ||
        n.find(L"lan ") != std::wstring::npos || n.find(L"802.11") != std::wstring::npos ||
        n.find(L"family controller") != std::wstring::npos) {
        return "Network";
    }

    // 3. USB Controllers
    if (g == L"{36fc9e60-c465-11cf-8056-444553540000}" || c == L"usb" ||
        h.find(L"&cc_0c03") != std::wstring::npos ||
        n.find(L"xhci") != std::wstring::npos || n.find(L"extensible host") != std::wstring::npos ||
        n.find(L"usb 3") != std::wstring::npos || n.find(L"usb host") != std::wstring::npos) {
        return "USB";
    }

    // 4. Audio
    if (g == L"{4d36e96c-e325-11ce-bfc1-08002be10318}" || c == L"media" ||
        h.find(L"&cc_04") != std::wstring::npos ||
        h.find(L"&cc_0403") != std::wstring::npos ||
        n.find(L"high definition audio") != std::wstring::npos ||
        n.find(L"audio controller") != std::wstring::npos ||
        n.find(L"sound") != std::wstring::npos || n.find(L"realtek audio") != std::wstring::npos ||
        n.find(L"hdaudbus") != std::wstring::npos) {
        return "Audio";
    }

    // 5. Storage (NVMe, SATA AHCI, RAID)
    if (g == L"{4d36e97b-e325-11ce-bfc1-08002be10318}" || g == L"{4d36e96a-e325-11ce-bfc1-08002be10318}" ||
        c == L"scsiadapter" || c == L"hdc" ||
        h.find(L"&cc_01") != std::wstring::npos ||
        n.find(L"nvm express") != std::wstring::npos || n.find(L"nvme") != std::wstring::npos ||
        n.find(L"sata") != std::wstring::npos || n.find(L"ahci") != std::wstring::npos ||
        n.find(L"storage controller") != std::wstring::npos || n.find(L"raid") != std::wstring::npos) {
        return "Storage";
    }

    return "Other";
}

static std::string SanitizeDeviceId(const std::string& id) {
    std::string cleanId = id;
    if (cleanId.rfind("PCI\\", 0) == 0 || cleanId.rfind("PCI/", 0) == 0) {
        cleanId = cleanId.substr(4);
    }
    while (!cleanId.empty() && (cleanId.front() == '\\' || cleanId.front() == '/')) {
        cleanId.erase(cleanId.begin());
    }
    return cleanId;
}

MsiManager::MsiManager() = default;

MsiManager& MsiManager::Get() {
    static MsiManager instance;
    return instance;
}

void MsiManager::ScanDevices() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ScanDevicesLocked();
}

void MsiManager::ScanDevicesLocked() {
    m_devices.clear();

    // Query Device Manager using SetupAPI: DIGCF_PRESENT strictly guarantees
    // only devices currently connected to the machine are enumerated.
    ScopedHDevInfo hDevInfo(SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT));
    if (!hDevInfo.isValid()) {
        m_scanned = true;
        return;
    }

    SP_DEVINFO_DATA devInfoData;
    ZeroMemory(&devInfoData, sizeof(devInfoData));
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD devIndex = 0; SetupDiEnumDeviceInfo(hDevInfo, devIndex, &devInfoData); ++devIndex) {
        // Query devnode status from Configuration Manager (PnP)
        ULONG status = 0;
        ULONG problem = 0;
        CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, devInfoData.DevInst, 0);
        if (cr != CR_SUCCESS) {
            // Not an active/valid device node
            continue;
        }

        // Skip disabled devices in Device Manager
        if (problem == CM_PROB_DISABLED || problem == CM_PROB_HARDWARE_DISABLED) {
            continue;
        }

        // Retrieve Device Instance ID (e.g. "PCI\VEN_10DE&DEV_2782&SUBSYS_16D710DE&REV_A1\4&2C8C4DA0&0&0008")
        wchar_t szInstanceId[MAX_DEVICE_ID_LEN] = { 0 };
        if (!SetupDiGetDeviceInstanceIdW(hDevInfo, &devInfoData, szInstanceId, MAX_DEVICE_ID_LEN, nullptr)) {
            continue;
        }

        // Friendly Name
        wchar_t friendly[512] = { 0 };
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_FRIENDLYNAME, nullptr, (PBYTE)friendly, sizeof(friendly), nullptr);

        // Device Description
        wchar_t desc[512] = { 0 };
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_DEVICEDESC, nullptr, (PBYTE)desc, sizeof(desc), nullptr);

        // Class Name
        wchar_t cls[256] = { 0 };
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_CLASS, nullptr, (PBYTE)cls, sizeof(cls), nullptr);

        // Class GUID
        wchar_t clsGuid[128] = { 0 };
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_CLASSGUID, nullptr, (PBYTE)clsGuid, sizeof(clsGuid), nullptr);

        // Location Info
        wchar_t loc[256] = { 0 };
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_LOCATION_INFORMATION, nullptr, (PBYTE)loc, sizeof(loc), nullptr);

        // Hardware ID
        wchar_t hwId[512] = { 0 };
        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_HARDWAREID, nullptr, (PBYTE)hwId, sizeof(hwId), nullptr);

        PciMsiDevice dev;
        dev.id = WideToUtf8(szInstanceId);
        dev.name = WideToUtf8(ExtractCleanName(desc, friendly));
        if (dev.name.empty()) {
            dev.name = dev.id;
        }
        dev.category = ClassifyDevice(cls, clsGuid, Utf8ToWide(dev.name), hwId);
        dev.location = WideToUtf8(CleanLocationInfo(loc));
        dev.hardwareId = WideToUtf8(hwId);

        // Check critical infrastructure devices that should not be modified
        std::wstring lowerName = Utf8ToWide(dev.name);
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);
        bool isBridgeOrPort = (lowerName.find(L"root port") != std::wstring::npos ||
                               lowerName.find(L"host cpu bridge") != std::wstring::npos ||
                               lowerName.find(L"isa bridge") != std::wstring::npos ||
                               lowerName.find(L"pci-to-pci") != std::wstring::npos ||
                               lowerName.find(L"switch port") != std::wstring::npos ||
                               lowerName.find(L"root complex") != std::wstring::npos ||
                               lowerName.find(L"system management") != std::wstring::npos);

        dev.canModify = (dev.category != "Other") && !isBridgeOrPort;

        // Inspect MSI properties key in Registry
        std::string cleanId = SanitizeDeviceId(dev.id);
        std::wstring devSubKey = L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\" + Utf8ToWide(cleanId);

        std::wstring msiKeyPath = devSubKey + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
        ScopedHKey hMsi;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, msiKeyPath.c_str(), 0, KEY_READ, hMsi.put()) == ERROR_SUCCESS) {
            dev.msiSupported = true;
            DWORD msiVal = 0;
            if (ReadRegDword(hMsi, L"MSISupported", msiVal)) {
                dev.msiEnabled = (msiVal == 1);
            }
            DWORD limitVal = 0;
            if (ReadRegDword(hMsi, L"MessageNumberLimit", limitVal)) {
                dev.messageLimit = (int)limitVal;
            }
        } else {
            // For modifiable PCIe devices, MSI can be enabled via registry key creation
            dev.msiSupported = dev.canModify;
            dev.msiEnabled = false;
            dev.messageLimit = 1;
        }

        // Inspect Affinity Policy
        std::wstring affKeyPath = devSubKey + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";
        ScopedHKey hAff;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, affKeyPath.c_str(), 0, KEY_READ, hAff.put()) == ERROR_SUCCESS) {
            DWORD prio = 0;
            if (ReadRegDword(hAff, L"DevicePriority", prio)) {
                dev.priority = (int)prio;
            }
        } else {
            dev.priority = 0;
        }

        m_devices.push_back(std::move(dev));
    }

    // Sort: Modifiable devices first, ordered by category importance (GPU -> Network -> USB -> Audio -> Storage -> Other)
    auto getCatWeight = [](const std::string& cat) {
        if (cat == "GPU") return 0;
        if (cat == "Network") return 1;
        if (cat == "USB") return 2;
        if (cat == "Audio") return 3;
        if (cat == "Storage") return 4;
        return 5;
    };

    std::sort(m_devices.begin(), m_devices.end(), [&](const PciMsiDevice& a, const PciMsiDevice& b) {
        if (a.canModify != b.canModify) return a.canModify > b.canModify;
        int wA = getCatWeight(a.category);
        int wB = getCatWeight(b.category);
        if (wA != wB) return wA < wB;
        return a.name < b.name;
    });

    m_scanned = true;
}

nlohmann::json MsiManager::GetDevicesJson() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_scanned) {
        ScanDevicesLocked();
    }

    nlohmann::json root;
    nlohmann::json devList = nlohmann::json::array();
    int msiCount = 0;

    for (const auto& d : m_devices) {
        if (d.msiEnabled) msiCount++;
        nlohmann::json item;
        item["id"] = d.id;
        item["name"] = d.name;
        item["category"] = d.category;
        item["msiSupported"] = d.msiSupported;
        item["msiEnabled"] = d.msiEnabled;
        item["messageLimit"] = d.messageLimit;
        item["priority"] = d.priority;
        item["canModify"] = d.canModify;
        item["location"] = d.location;
        item["hardwareId"] = d.hardwareId;
        devList.push_back(item);
    }

    root["devices"] = devList;
    root["total"] = (int)m_devices.size();
    root["msiCount"] = msiCount;
    return root;
}

bool MsiManager::SetMsiEnabled(const std::string& id, bool enabled, std::string& outMsg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string cleanId = SanitizeDeviceId(id);

    // Validate that device is safe to modify
    PciMsiDevice* targetDev = nullptr;
    for (auto& d : m_devices) {
        if (d.id == id || d.id == cleanId || SanitizeDeviceId(d.id) == cleanId) {
            targetDev = &d;
            break;
        }
    }
    if (targetDev && !targetDev->canModify) {
        outMsg = "Устройство является критическим системным контроллером и защищено от изменений.";
        return false;
    }

    std::wstring devSubKey = L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\" + Utf8ToWide(cleanId);
    std::wstring msiKeyPath = devSubKey + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";

    ScopedHKey hKey;
    DWORD disp = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, msiKeyPath.c_str(), 0, NULL,
                                     REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, hKey.put(), &disp);
    if (status != ERROR_SUCCESS) {
        outMsg = "Ошибка доступа к реестру (код " + std::to_string(status) + ")";
        return false;
    }

    DWORD val = enabled ? 1 : 0;
    status = RegSetValueExW(hKey, L"MSISupported", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    if (status != ERROR_SUCCESS) {
        outMsg = "Не удалось записать значение MSISupported";
        return false;
    }

    if (enabled) {
        DWORD msgLimit = 0;
        DWORD cbData = sizeof(msgLimit);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"MessageNumberLimit", NULL, &type, (LPBYTE)&msgLimit, &cbData) != ERROR_SUCCESS || msgLimit == 0) {
            msgLimit = 1;
            RegSetValueExW(hKey, L"MessageNumberLimit", 0, REG_DWORD, (const BYTE*)&msgLimit, sizeof(msgLimit));
        }
    }

    // Update in-memory device list
    for (auto& d : m_devices) {
        if (d.id == id || d.id == cleanId || SanitizeDeviceId(d.id) == cleanId) {
            d.msiEnabled = enabled;
            d.msiSupported = true;
            if (enabled && d.messageLimit <= 0) d.messageLimit = 1;
            break;
        }
    }

    outMsg = "Режим MSI успешно " + std::string(enabled ? "включен" : "отключен") + ". Требуется перезагрузка для применения драйвером.";
    return true;
}

bool MsiManager::SetDevicePriority(const std::string& id, int priority, std::string& outMsg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string cleanId = SanitizeDeviceId(id);

    // Validate that device is safe to modify
    PciMsiDevice* targetDev = nullptr;
    for (auto& d : m_devices) {
        if (d.id == id || d.id == cleanId || SanitizeDeviceId(d.id) == cleanId) {
            targetDev = &d;
            break;
        }
    }
    if (targetDev && !targetDev->canModify) {
        outMsg = "Устройство является критическим системным контроллером и защищено от изменений.";
        return false;
    }

    std::wstring devSubKey = L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\" + Utf8ToWide(cleanId);
    std::wstring affKeyPath = devSubKey + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";

    ScopedHKey hKey;
    DWORD disp = 0;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, affKeyPath.c_str(), 0, NULL,
                                     REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, hKey.put(), &disp);
    if (status != ERROR_SUCCESS) {
        outMsg = "Ошибка доступа к реестру Affinity Policy (код " + std::to_string(status) + ")";
        return false;
    }

    if (priority == 0) {
        RegDeleteValueW(hKey, L"DevicePriority");
    } else {
        DWORD val = (DWORD)priority;
        RegSetValueExW(hKey, L"DevicePriority", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    }

    for (auto& d : m_devices) {
        if (d.id == id || d.id == cleanId || SanitizeDeviceId(d.id) == cleanId) {
            d.priority = priority;
            break;
        }
    }

    const char* prioNames[] = { "По умолчанию", "Низкий", "Обычный", "Высокий" };
    const char* pName = (priority >= 0 && priority <= 3) ? prioNames[priority] : "По умолчанию";
    outMsg = "Приоритет IRQ установлен: " + std::string(pName) + ".";
    return true;
}

bool MsiManager::ApplyRecommendedEsportsProfile(std::string& outMsg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_scanned) {
        ScanDevicesLocked();
    }

    int gpuCount = 0;
    int netCount = 0;
    int usbCount = 0;
    int audioCount = 0;

    auto setMsi = [](const std::string& devId, bool enabled) {
        std::string cleanId = SanitizeDeviceId(devId);
        std::wstring path = L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\" + Utf8ToWide(cleanId) + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
        ScopedHKey hKey;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, hKey.put(), NULL) == ERROR_SUCCESS) {
            DWORD val = enabled ? 1 : 0;
            RegSetValueExW(hKey, L"MSISupported", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
            DWORD msgLimit = 0, cbData = sizeof(msgLimit), type = 0;
            if (RegQueryValueExW(hKey, L"MessageNumberLimit", NULL, &type, (LPBYTE)&msgLimit, &cbData) != ERROR_SUCCESS || msgLimit == 0) {
                msgLimit = 1;
                RegSetValueExW(hKey, L"MessageNumberLimit", 0, REG_DWORD, (const BYTE*)&msgLimit, sizeof(msgLimit));
            }
        }
    };

    auto setPrio = [](const std::string& devId, int priority) {
        std::string cleanId = SanitizeDeviceId(devId);
        std::wstring path = L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\" + Utf8ToWide(cleanId) + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";
        ScopedHKey hKey;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, hKey.put(), NULL) == ERROR_SUCCESS) {
            if (priority == 0) {
                RegDeleteValueW(hKey, L"DevicePriority");
            } else {
                DWORD val = (DWORD)priority;
                RegSetValueExW(hKey, L"DevicePriority", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
            }
        }
    };

    for (auto& d : m_devices) {
        if (!d.canModify) continue;

        if (d.category == "GPU") {
            setMsi(d.id, true);
            setPrio(d.id, 3); // High
            d.msiEnabled = true;
            d.msiSupported = true;
            d.priority = 3;
            gpuCount++;
        } else if (d.category == "Network") {
            setMsi(d.id, true);
            setPrio(d.id, 3); // High
            d.msiEnabled = true;
            d.msiSupported = true;
            d.priority = 3;
            netCount++;
        } else if (d.category == "USB") {
            setMsi(d.id, true);
            setPrio(d.id, 3); // High
            d.msiEnabled = true;
            d.msiSupported = true;
            d.priority = 3;
            usbCount++;
        } else if (d.category == "Audio") {
            setMsi(d.id, true);
            setPrio(d.id, 2); // Normal
            d.msiEnabled = true;
            d.msiSupported = true;
            d.priority = 2;
            audioCount++;
        }
    }

    outMsg = "Киберспортивный профиль MSI успешно применен! GPU (" + std::to_string(gpuCount) +
             ", High), Сеть (" + std::to_string(netCount) + ", High), USB (" + std::to_string(usbCount) +
             ", High), Звук (" + std::to_string(audioCount) + ", Normal). Требуется перезагрузка ПК.";
    return true;
}

bool MsiManager::IsCategoryMsiEnabled(const std::string& category) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_scanned) {
        ScanDevicesLocked();
    }
    for (const auto& d : m_devices) {
        if (d.category == category && d.msiEnabled) {
            return true;
        }
    }
    return false;
}

bool MsiManager::SetCategoryMsi(const std::string& category, bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_scanned) {
        ScanDevicesLocked();
    }
    bool anyChanged = false;
    for (auto& d : m_devices) {
        if (d.category == category && d.canModify) {
            std::string cleanId = SanitizeDeviceId(d.id);
            std::wstring msiPath = L"SYSTEM\\CurrentControlSet\\Enum\\PCI\\" + Utf8ToWide(cleanId) + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
            ScopedHKey hKey;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, msiPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, hKey.put(), NULL) == ERROR_SUCCESS) {
                DWORD val = enabled ? 1 : 0;
                RegSetValueExW(hKey, L"MSISupported", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
                if (enabled) {
                    DWORD limit = 1;
                    RegSetValueExW(hKey, L"MessageNumberLimit", 0, REG_DWORD, (const BYTE*)&limit, sizeof(limit));
                }
                d.msiEnabled = enabled;
                d.msiSupported = true;
                anyChanged = true;
            }
        }
    }
    return anyChanged;
}


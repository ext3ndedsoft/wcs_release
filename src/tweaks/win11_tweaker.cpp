#include "win11_tweaker.h"
#include "common_utils.h"
#include "debug_logger.h"
#include <shlobj.h>

typedef struct _WIN11_RTL_OSVERSIONINFOW {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    WCHAR szCSDVersion[128];
} WIN11_RTL_OSVERSIONINFOW, *PWIN11_RTL_OSVERSIONINFOW;

typedef LONG(NTAPI* pfnRtlGetVersion)(PWIN11_RTL_OSVERSIONINFOW);

Win11Tweaker& Win11Tweaker::Get() {
    static Win11Tweaker instance;
    return instance;
}

Win11Tweaker::Win11Tweaker() {
    Init();
}

void Win11Tweaker::Init() {
    DetectOSVersion();
    InitDatabase();
    DetectState();
}

void Win11Tweaker::DetectOSVersion() {
    m_buildNumber = 0;
    m_majorVersion = 0;
    m_minorVersion = 0;

    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto pRtlGetVersion = reinterpret_cast<pfnRtlGetVersion>(GetProcAddress(hNtdll, "RtlGetVersion"));
        if (pRtlGetVersion) {
            WIN11_RTL_OSVERSIONINFOW rovi = { 0 };
            rovi.dwOSVersionInfoSize = sizeof(rovi);
            if (pRtlGetVersion(&rovi) == 0) {
                m_buildNumber = rovi.dwBuildNumber;
                m_majorVersion = rovi.dwMajorVersion;
                m_minorVersion = rovi.dwMinorVersion;
            }
        }
    }

    if (m_buildNumber == 0) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t buildBuf[64] = { 0 };
            DWORD size = sizeof(buildBuf);
            if (RegQueryValueExW(hKey, L"CurrentBuildNumber", nullptr, nullptr, (LPBYTE)buildBuf, &size) == ERROR_SUCCESS) {
                m_buildNumber = static_cast<DWORD>(_wtoi(buildBuf));
            }
            RegCloseKey(hKey);
        }
    }

    m_isWindows11 = (m_buildNumber >= 22000);
}

void Win11Tweaker::InitDatabase() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();

    m_items.push_back({
        "win11ClassicMenu",
        "Классическое контекстное меню Windows 11",
        "Возврат быстрого классического контекстного меню проводника без задержки и без меню «Показать дополнительные параметры».",
        false,
        true
    });

    m_items.push_back({
        "win11Copilot",
        "Отключение Windows Copilot и фоновых AI-процессов",
        "GPO-политика блокировки ассистента Windows Copilot и предотвращения автозапуска его фоновых AI-процессов.",
        false,
        true
    });

    m_items.push_back({
        "win11Recall",
        "Блокировка Windows Recall (AI скриншоты в 24H2)",
        "Отключение компонента Windows Recall, предотвращающее скрытый захват снимков экрана и их анализ локальным ИИ.",
        false,
        true
    });

    m_items.push_back({
        "win11BingSearch",
        "Отключение веб-поиска Bing в меню «Пуск»",
        "Отключение веб-поиска Bing и подсказок в меню «Пуск», ускоряя локальный поиск файлов.",
        false,
        false
    });

    m_items.push_back({
        "win11StartAds",
        "Отключение рекламы и рекомендаций в меню «Пуск»",
        "Отключение рекламы, рекомендаций и Iris-промо в меню «Пуск» Windows 11.",
        false,
        true
    });

    m_items.push_back({
        "win11Widgets",
        "Отключение панели виджетов и процесса Widgets.exe",
        "Отключение панели виджетов и процесса Widgets.exe, экономия оперативной памяти и скрытие иконки с панели задач.",
        false,
        true
    });

    m_items.push_back({
        "win11ConsumerBloat",
        "Блокировка фоновой докачки спонсорского ПО",
        "GPO-политика запрета автоматической фоновой установки рекомендованных партнерских приложений и игр.",
        false,
        false
    });

    m_items.push_back({
        "win11TypingTelemetry",
        "Отключение сбора телеметрии клавиатуры и рукописного ввода",
        "Запрет скрытой отправки текста, словаря пользователя и телеметрии рукописного ввода в облако Microsoft.",
        false,
        false
    });
}

void Win11Tweaker::DetectState() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& it : m_items) {
        if (it.id == "win11ClassicMenu") {
            HKEY hKey = nullptr;
            bool ok = false;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                wchar_t val[16] = { 0 };
                DWORD size = sizeof(val);
                if (RegQueryValueExW(hKey, L"", nullptr, nullptr, (LPBYTE)val, &size) == ERROR_SUCCESS) {
                    ok = (val[0] == L'\0');
                } else {
                    ok = true;
                }
                RegCloseKey(hKey);
            }
            it.enabled = ok;
        }
        else if (it.id == "win11Copilot") {
            DWORD v1 = 0, v2 = 0;
            bool b1 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot", v1) && (v1 == 1);
            bool b2 = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot", v2) && (v2 == 1);
            it.enabled = (b1 || b2);
        }
        else if (it.id == "win11Recall") {
            DWORD v1 = 0, v2 = 0;
            bool b1 = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows AI", L"DisableAIDataAnalysis", v1) && (v1 == 1);
            bool b2 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\Windows AI", L"DisableAIDataAnalysis", v2) && (v2 == 1);
            it.enabled = (b1 || b2);
        }
        else if (it.id == "win11BingSearch") {
            DWORD v1 = 0, v2 = 1;
            bool b1 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\Explorer", L"DisableSearchBoxSuggestions", v1) && (v1 == 1);
            bool b2 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BingSearchEnabled", v2) && (v2 == 0);
            it.enabled = (b1 || b2);
        }
        else if (it.id == "win11StartAds") {
            DWORD val = 1;
            it.enabled = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_IrisRecommendations", val) && (val == 0);
        }
        else if (it.id == "win11Widgets") {
            DWORD v1 = 1, v2 = 1, v3 = 1;
            bool b1 = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests", v1) && (v1 == 0);
            bool b2 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests", v2) && (v2 == 0);
            bool b3 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarDa", v3) && (v3 == 0);
            it.enabled = (b1 || b2 || b3);
        }
        else if (it.id == "win11ConsumerBloat") {
            DWORD val = 0;
            it.enabled = RegGetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableWindowsConsumerFeatures", val) && (val == 1);
        }
        else if (it.id == "win11TypingTelemetry") {
            DWORD v1 = 0, v2 = 0;
            bool b1 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitTextCollection", v1) && (v1 == 1);
            bool b2 = RegGetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection", v2) && (v2 == 1);
            it.enabled = (b1 || b2);
        }
    }
}

nlohmann::json Win11Tweaker::GetStateJson() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json root;
    root["success"] = true;
    root["isWin11"] = m_isWindows11;
    root["buildNumber"] = m_buildNumber;
    root["majorVersion"] = m_majorVersion;
    root["minorVersion"] = m_minorVersion;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : m_items) {
        nlohmann::json obj;
        obj["id"] = item.id;
        obj["name"] = item.name;
        obj["description"] = item.description;
        obj["enabled"] = item.enabled;
        obj["isWin11Only"] = item.isWin11Only;
        arr.push_back(obj);
    }
    root["tweaks"] = arr;
    return root;
}

bool Win11Tweaker::HasTweak(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& it : m_items) {
        if (it.id == id) return true;
    }
    return false;
}

bool Win11Tweaker::ToggleTweak(const std::string& id, bool enabled) {
    std::string dummy;
    return ToggleTweak(id, enabled, dummy);
}

bool Win11Tweaker::ToggleTweak(const std::string& id, bool enabled, std::string& outMsg) {
    std::lock_guard<std::mutex> lock(m_mutex);

    bool handled = false;
    if (id == "win11ClassicMenu") {
        if (enabled) {
            HKEY hKey = nullptr;
            DWORD disp = 0;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
                const wchar_t emptyStr[] = L"";
                RegSetValueExW(hKey, L"", 0, REG_SZ, (const BYTE*)emptyStr, sizeof(emptyStr));
                RegCloseKey(hKey);
            }
            outMsg = "Классическое контекстное меню Windows 11 включено";
        } else {
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}");
            outMsg = "Стандартное контекстное меню Windows 11 восстановлено";
        }
        handled = true;
    }
    else if (id == "win11Copilot") {
        if (enabled) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot", 1);
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot", 1);
            outMsg = "Windows Copilot и фоновые AI-процессы отключены (GPO HKLM & HKCU)";
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot");
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot");
            outMsg = "Windows Copilot восстановлен по умолчанию";
        }
        handled = true;
    }
    else if (id == "win11Recall") {
        if (enabled) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows AI", L"DisableAIDataAnalysis", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\Windows AI", L"DisableAIDataAnalysis", 1);
            outMsg = "Windows Recall заблокирован (DisableAIDataAnalysis = 1)";
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows AI", L"DisableAIDataAnalysis");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\Windows AI", L"DisableAIDataAnalysis");
            outMsg = "Windows Recall восстановлен по умолчанию";
        }
        handled = true;
    }
    else if (id == "win11BingSearch") {
        if (enabled) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\Explorer", L"DisableSearchBoxSuggestions", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BingSearchEnabled", 0);
            outMsg = "Веб-поиск Bing в меню «Пуск» отключен";
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\Explorer", L"DisableSearchBoxSuggestions");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BingSearchEnabled");
            outMsg = "Веб-поиск Bing в меню «Пуск» включен";
        }
        handled = true;
    }
    else if (id == "win11StartAds") {
        if (enabled) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_IrisRecommendations", 0);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338388Enabled", 0);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338389Enabled", 0);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SystemPaneSuggestionsEnabled", 0);
            outMsg = "Реклама и рекомендации в меню «Пуск» отключены (Start_IrisRecommendations = 0)";
        } else {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_IrisRecommendations", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338388Enabled", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338389Enabled", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SystemPaneSuggestionsEnabled", 1);
            outMsg = "Рекомендации в меню «Пуск» включены";
        }
        handled = true;
    }
    else if (id == "win11Widgets") {
        if (enabled) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests", 0);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests", 0);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarDa", 0);
            outMsg = "Панель виджетов и Widgets.exe отключены (AllowNewsAndInterests = 0)";
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Dsh", L"AllowNewsAndInterests");
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarDa", 1);
            outMsg = "Панель виджетов включена";
        }
        handled = true;
    }
    else if (id == "win11ConsumerBloat") {
        if (enabled) {
            RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableWindowsConsumerFeatures", 1);
            outMsg = "Фоновая докачка спонсорского ПО заблокирована";
        } else {
            RegDeleteVal(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableWindowsConsumerFeatures");
            outMsg = "Установка спонсорских приложений возвращена по умолчанию";
        }
        handled = true;
    }
    else if (id == "win11TypingTelemetry") {
        if (enabled) {
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitTextCollection", 1);
            RegSetDword(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection", 1);
            outMsg = "Телеметрия клавиатуры и рукописного ввода отключена";
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitTextCollection");
            RegDeleteVal(HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\InputPersonalization", L"RestrictImplicitInkCollection");
            outMsg = "Сбор данных ввода возвращен по умолчанию";
        }
        handled = true;
    }

    if (!handled) {
        outMsg = "Неизвестный твик Windows 11: " + id;
        return false;
    }

    for (auto& item : m_items) {
        if (item.id == id) {
            item.enabled = enabled;
            break;
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    DebugLogger::Get().LogTweak(id, enabled, outMsg);
    return true;
}

bool Win11Tweaker::RegSetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD val) {
    bool ok = RegUtil::SetDword(hRoot, subKey, valName, val);
    std::wstring rootName = (hRoot == HKEY_LOCAL_MACHINE) ? L"HKLM" : ((hRoot == HKEY_CURRENT_USER) ? L"HKCU" : L"HKCR");
    DebugLogger::Get().LogRegistry(rootName, subKey, valName, val, ok);
    return ok;
}

bool Win11Tweaker::RegDeleteVal(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName) {
    return RegUtil::DeleteVal(hRoot, subKey, valName);
}

bool Win11Tweaker::RegGetDword(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName, DWORD& outVal) {
    return RegUtil::GetDword(hRoot, subKey, valName, outVal);
}

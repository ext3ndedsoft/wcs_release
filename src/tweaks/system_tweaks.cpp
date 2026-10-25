#include "system_tweaks.h"
#include "common_utils.h"
#include "debug_logger.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>

ContextMenuManager& ContextMenuManager::Get() {
    static ContextMenuManager instance;
    return instance;
}

ContextMenuManager::ContextMenuManager() {
    m_items = {
        // --- Main 16 Tweaks ---
        { "emptyFolderContent", "Удалить содержимое папки", "Очищение папки от всех файлов и вложенных каталогов без удаления самой папки.", false, false },
        { "symlinkCreate", "Создать символическую ссылку", "Создание жестких и символических ссылок на файлы и каталоги в один клик.", false, false },
        { "takeOwnershipUser", "Стать владельцем и получить пользовательский доступ", "Получение полных прав владения и доступа текущего пользователя (takeown + icacls).", false, false },
        { "takeOwnershipAdmin", "Стать владельцем и получить доступ администратора", "Предоставление полных прав Администратора на защищенные системные файлы и папки.", false, false },
        { "blockNetAccess", "Запретить/Разрешить приложению доступ в Интернет", "Мгновенная блокировка или разблокировка трафика в Windows Defender Firewall.", false, false },
        { "cmdPsAnywhere", "Открыть окна CMD и PowerShell из любого места", "Быстрый запуск командной строки CMD и PowerShell в текущей папке.", false, false },
        { "installCab", "Установить CAB", "Инсталляция пакетов обновлений .cab через Dism /Online /Add-Package.", false, false },
        { "hashFile", "Хеш-суммы файла", "Мгновенное вычисление и отображение SHA256, MD5 и SHA1 контрольных сумм.", false, false },
        { "compactSeparate", "Сжать отдельно файл/директорию", "Аппаратное сжатие NTFS/CompactOS (XPRESS / LZX) для освобождения места.", false, false },
        { "copyImageBuffer", "Копировать в буфер на изображениях", "Копирование содержимого графического файла напрямую в системный буфер обмена.", false, false },
        { "permanentDelete", "Удалить окончательно", "Удаление файла или директории мимо Корзины без возможности восстановления.", false, false },
        { "cleanJunkContext", "Очистка системы, папок и реестра от мусора", "Интеграция в контекстное меню проводника комплексной очистки Windows Config Studio.", false, false },
        { "taskMgrContext", "Диспетчер задач в контекстное меню", "Быстрый вызов Task Manager из контекстного меню рабочего стола и проводника.", false, false },

        // --- Sub-Panel 1: Компьютер (19 items) ---
        { "comp-admin", "Администрирование", "Контекстное меню Компьютера: Администрирование", false, false },
        { "comp-devmgmt", "Диспетчер устройств", "Контекстное меню Компьютера: Диспетчер устройств", false, false },
        { "comp-control", "Панель управления", "Контекстное меню Компьютера: Панель управления", false, false },
        { "comp-appwiz", "Программы и компоненты", "Контекстное меню Компьютера: Программы и компоненты", false, false },
        { "comp-gpedit", "Редактор групповой политики", "Контекстное меню Компьютера: Редактор групповой политики", false, false },
        { "comp-regedit", "Редактор реестра", "Контекстное меню Компьютера: Редактор реестра", false, false },
        { "comp-services", "Службы", "Контекстное меню Компьютера: Службы", false, false },
        { "comp-diskmgmt", "Управление дисками", "Контекстное меню Компьютера: Управление дисками", false, false },
        { "comp-msconfig", "Конфигурация системы", "Контекстное меню Компьютера: Конфигурация системы", false, false },
        { "comp-taskschd", "Планировщик заданий", "Контекстное меню Компьютера: Планировщик заданий", false, false },
        { "comp-winver", "Версия Windows", "Контекстное меню Компьютера: Версия Windows", false, false },
        { "comp-eventvwr", "Просмотр событий", "Контекстное меню Компьютера: Просмотр событий", false, false },
        { "comp-sysprops", "Свойства системы (Классическое)", "Контекстное меню Компьютера: Свойства системы", false, false },
        { "comp-replace-props", "Замена Свойства на классическое", "Контекстное меню Компьютера: Замена Свойства на классическое", false, false },
        { "comp-hosts", "Редактирование файла Hosts", "Контекстное меню Компьютера: Редактирование файла Hosts", false, false },
        { "comp-netconnections", "Сетевые подключения", "Контекстное меню Компьютера: Сетевые подключения", false, false },
        { "comp-perfmon", "Монитор стабильности системы", "Контекстное меню Компьютера: Монитор стабильности системы", false, false },
        { "comp-resmon", "Монитор ресурсов системы", "Контекстное меню Компьютера: Монитор ресурсов системы", false, false },
        { "comp-godmode", "Все задачи", "Контекстное меню Компьютера: Все задачи (God Mode)", false, false },

        // --- Sub-Panel 2: Рабочий стол (Добавление - 16 items) ---
        { "desk-restart-explorer", "Перезапуск проводника", "Меню рабочего стола: Перезапуск проводника", false, false },
        { "desk-refresh-icons", "Обновить кэш иконок", "Меню рабочего стола: Обновить кэш иконок", false, false },
        { "desk-compact-os", "Проверка наличия сжатия ОС", "Меню рабочего стола: Проверка наличия сжатия ОС", false, false },
        { "desk-power-schemes", "Схемы управления питанием", "Меню рабочего стола: Схемы управления питанием", false, false },
        { "desk-safe-mode", "Безопасный режим", "Меню рабочего стола: Безопасный режим", false, false },
        { "desk-toggle-net", "Отключить/Включить интернет", "Меню рабочего стола: Отключить/Включить интернет", false, false },
        { "desk-toggle-hidden", "Показать/Скрыть скрытые файлы и папки", "Меню рабочего стола: Показать/Скрыть скрытые файлы и папки", false, false },
        { "desk-toggle-ext", "Показать/Скрыть расширения файлов", "Меню рабочего стола: Показать/Скрыть расширения файлов", false, false },
        { "desk-safe-remove", "Безопасное извлечение", "Меню рабочего стола: Безопасное извлечение", false, false },
        { "desk-screen-clip", "Набросок на фрагменте экрана", "Меню рабочего стола: Набросок на фрагменте экрана", false, false },
        { "desk-runas-ti", "Запускать от имени TI", "Меню рабочего стола: Запускать от имени TI", false, false },
        { "desk-theme-toggle", "Тема День/Ночь", "Меню рабочего стола: Переключение темы День/Ночь", false, false },
        { "desk-display-switch", "Переключение экранов с двумя мониторами", "Меню рабочего стола: Переключение экранов", false, false },
        { "desk-screen-off", "Отключить экран", "Меню рабочего стола: Отключить экран", false, false },
        { "desk-power-menu", "Выключить или перезагрузить компьютер", "Меню рабочего стола: Завершение работы", false, false },
        { "desk-classic-personalize", "Улучшить пункт «Персонализация»", "Меню рабочего стола: Классическая персонализация", false, false },

        // --- Sub-Panel 3: Рабочий стол (Удаление - 4 items) ---
        { "del-amd", "AMD Software", "Скрытие пункта AMD Software из контекстного меню рабочего стола", false, false },
        { "del-intel", "Intel Graphics Options", "Скрытие пункта Intel Graphics Options из контекстного меню", false, false },
        { "del-nvidia", "Nvidia Graphics Options", "Скрытие пункта Nvidia Graphics Options из контекстного меню", false, false },
        { "del-vs", "Visual Studio", "Скрытие пунктов Visual Studio из контекстного меню рабочего стола", false, false },

        // --- Sub-Panel 4: Меню Создать (11 items) ---
        { "new-del-bmp", "Точечный рисунок", "Удаление точечного рисунка из меню Создать", false, false },
        { "new-del-contact", "Контакт", "Удаление контакта из меню Создать", false, false },
        { "new-del-rtf", "Документ в формате RTF", "Удаление RTF из меню Создать", false, false },
        { "new-del-txt", "Текстовый документ", "Удаление текстового документа из меню Создать", false, false },
        { "new-del-zip", "Сжатая ZIP-папка", "Удаление ZIP-папки из меню Создать", false, false },
        { "new-del-briefcase", "Портфель", "Удаление Портфеля из меню Создать", true, false },
        { "new-add-cmd", "Сценарий Windows (.cmd)", "Добавление .cmd в меню Создать", false, false },
        { "new-add-reg", "Файл реестра (.reg)", "Добавление .reg в меню Создать", false, false },
        { "new-add-vbs", "Файл сценария VBScript (.vbs)", "Добавление .vbs в меню Создать", false, false },
        { "new-add-bat", "Пакетный файл Windows (.bat)", "Добавление .bat в меню Создать", false, false },
        { "new-add-ps1", "Сценарий PowerShell (.ps1)", "Добавление .ps1 в меню Создать", false, false },

        // --- Sub-Panel 5: Удаление мусора (13 items) ---
        { "junk-previous-versions", "Восстановить предыдущую версию", "Удаление пункта «Восстановить предыдущую версию»", false, false },
        { "junk-paint-3d", "Изменить в Paint 3D/Print 3D", "Удаление Paint 3D и 3D Print из меню", false, false },
        { "junk-pin-start", "Закрепить на начальном экране", "Удаление пункта «Закрепить на начальном экране»", false, false },
        { "junk-pin-quickaccess", "Закрепить на панели быстрого доступа", "Удаление пункта «Закрепить на панели быстрого доступа»", false, false },
        { "junk-find-music", "Поиск музыки в Интернете", "Удаление устаревшего пункта «Поиск музыки в Интернете»", true, false },
        { "junk-share", "Отправить (поделиться)", "Удаление пункта «Поделиться» (Modern Share)", false, false },
        { "junk-cast", "Передать на устройство", "Удаление пункта «Передать на устройство» (PlayTo)", false, false },
        { "junk-library", "Добавить в библиотеку", "Удаление пункта «Добавить в библиотеку»", false, false },
        { "junk-map-drive", "Подключить/Отключить сетевой диск...", "Удаление пунктов сетевого диска", false, false },
        { "junk-extract-all", "Извлечь все... (Windows 11)", "Удаление пункта «Извлечь все...»", false, false },
        { "junk-add-favorites", "Добавить в избранное (Windows 11)", "Удаление пункта «Добавить в избранное»", false, false },
        { "junk-bitlocker", "Включить BitLocker", "Удаление пункта «Включить BitLocker»", false, false },
        { "junk-manage", "Управление", "Скрытие пункта «Управление» из меню Компьютера", false, false }
    };
    DetectState();
}

void ContextMenuManager::Init() {
    DetectState();
}

bool ContextMenuManager::RegKeyExists(HKEY hRoot, const std::wstring& subKey) {
    ScopedHKey hKey;
    return (RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_READ, hKey.put()) == ERROR_SUCCESS);
}

bool ContextMenuManager::RegValueExists(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName) {
    ScopedHKey hKey;
    if (RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_READ, hKey.put()) == ERROR_SUCCESS) {
        LSTATUS status = RegQueryValueExW(hKey, valName.c_str(), nullptr, nullptr, nullptr, nullptr);
        return (status == ERROR_SUCCESS);
    }
    return false;
}

bool ContextMenuManager::RegDeleteTreeNative(HKEY hRoot, const std::wstring& subKey) {
    return RegUtil::DeleteSubKeyTree(hRoot, subKey);
}

bool ContextMenuManager::SetShellVerb(HKEY hRoot, const std::wstring& path, const std::wstring& menuText,
                                      const std::wstring& cmd, const std::wstring& icon,
                                      bool shiftOnly, bool hasShield) {
    ScopedHKey hKey;
    LSTATUS status = RegCreateKeyExW(hRoot, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, hKey.put(), nullptr);
    if (status != ERROR_SUCCESS) return false;

    if (!menuText.empty()) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)menuText.c_str(), (DWORD)((menuText.size() + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"MUIVerb", 0, REG_SZ, (const BYTE*)menuText.c_str(), (DWORD)((menuText.size() + 1) * sizeof(wchar_t)));
    }

    if (!icon.empty()) {
        RegSetValueExW(hKey, L"Icon", 0, REG_SZ, (const BYTE*)icon.c_str(), (DWORD)((icon.size() + 1) * sizeof(wchar_t)));
    }

    if (hasShield) {
        const wchar_t emptyStr[] = L"";
        RegSetValueExW(hKey, L"HasLUAShield", 0, REG_SZ, (const BYTE*)emptyStr, sizeof(emptyStr));
    } else {
        RegDeleteValueW(hKey, L"HasLUAShield");
    }

    if (shiftOnly) {
        const wchar_t emptyStr[] = L"";
        RegSetValueExW(hKey, L"Extended", 0, REG_SZ, (const BYTE*)emptyStr, sizeof(emptyStr));
    } else {
        RegDeleteValueW(hKey, L"Extended");
    }

    // Set command subkey
    ScopedHKey hCmdKey;
    std::wstring cmdPath = path + L"\\command";
    if (RegCreateKeyExW(hRoot, cmdPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, hCmdKey.put(), nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hCmdKey, nullptr, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }

    return true;
}

bool ContextMenuManager::RemoveShellVerb(HKEY hRoot, const std::wstring& path) {
    bool ok1 = RegDeleteTreeNative(hRoot, path);
    bool ok2 = RegDeleteTreeNative(HKEY_CURRENT_USER, L"Software\\Classes\\" + path);
    return ok1 || ok2;
}

bool ContextMenuManager::UpdateExtendedFlag(HKEY hRoot, const std::wstring& path, bool shiftOnly) {
    if (shiftOnly) {
        return RegSetString(hRoot, path, L"Extended", L"");
    } else {
        return RegDeleteVal(hRoot, path, L"Extended");
    }
}

bool ContextMenuManager::SetProgrammaticAccessOnly(HKEY hRoot, const std::wstring& keyPath, bool hide) {
    bool ok = hide ? RegSetString(hRoot, keyPath, L"ProgrammaticAccessOnly", L"")
                   : RegDeleteVal(hRoot, keyPath, L"ProgrammaticAccessOnly");
    if (hRoot == HKEY_CLASSES_ROOT) {
        std::wstring cuPath = L"Software\\Classes\\" + keyPath;
        if (hide) {
            RegSetString(HKEY_CURRENT_USER, cuPath, L"ProgrammaticAccessOnly", L"");
        } else {
            RegDeleteVal(HKEY_CURRENT_USER, cuPath, L"ProgrammaticAccessOnly");
        }
    }
    return ok;
}

bool ContextMenuManager::HasProgrammaticAccessOnly(HKEY hRoot, const std::wstring& keyPath) {
    if (RegValueExists(hRoot, keyPath, L"ProgrammaticAccessOnly")) return true;
    if (hRoot == HKEY_CLASSES_ROOT) {
        return RegValueExists(HKEY_CURRENT_USER, L"Software\\Classes\\" + keyPath, L"ProgrammaticAccessOnly");
    }
    return false;
}

bool ContextMenuManager::ToggleShellexHandler(HKEY hRoot, const std::wstring& parentPath, const std::wstring& handlerName, bool hide) {
    std::wstring normalPath = parentPath + L"\\" + handlerName;
    std::wstring hiddenPath = parentPath + L"\\-" + handlerName;
    if (hide) {
        if (RegKeyExists(hRoot, normalPath)) {
            ScopedHKey hSrc, hDst;
            if (RegOpenKeyExW(hRoot, normalPath.c_str(), 0, KEY_READ, hSrc.put()) == ERROR_SUCCESS) {
                if (RegCreateKeyExW(hRoot, hiddenPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, hDst.put(), nullptr) == ERROR_SUCCESS) {
                    RegCopyTreeW(hSrc, nullptr, hDst);
                }
                hDst.Close();
                hSrc.Close();
                RegDeleteTreeNative(hRoot, normalPath);
            }
        }
        return true;
    } else {
        if (RegKeyExists(hRoot, hiddenPath)) {
            ScopedHKey hSrc, hDst;
            if (RegOpenKeyExW(hRoot, hiddenPath.c_str(), 0, KEY_READ, hSrc.put()) == ERROR_SUCCESS) {
                if (RegCreateKeyExW(hRoot, normalPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, hDst.put(), nullptr) == ERROR_SUCCESS) {
                    RegCopyTreeW(hSrc, nullptr, hDst);
                }
                hDst.Close();
                hSrc.Close();
                RegDeleteTreeNative(hRoot, hiddenPath);
            }
        }
        return true;
    }
}

bool ContextMenuManager::IsShellexHandlerHidden(HKEY hRoot, const std::wstring& parentPath, const std::wstring& handlerName) {
    std::wstring hiddenPath = parentPath + L"\\-" + handlerName;
    return RegKeyExists(hRoot, hiddenPath);
}

bool ContextMenuManager::ToggleShellNew(const std::wstring& ext, bool disable) {
    std::wstring normalPath = ext + L"\\ShellNew";
    std::wstring hiddenPath = ext + L"\\ShellNew_Disabled";
    if (disable) {
        if (RegKeyExists(HKEY_CLASSES_ROOT, normalPath)) {
            ScopedHKey hSrc, hDst;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, normalPath.c_str(), 0, KEY_READ, hSrc.put()) == ERROR_SUCCESS) {
                if (RegCreateKeyExW(HKEY_CLASSES_ROOT, hiddenPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, hDst.put(), nullptr) == ERROR_SUCCESS) {
                    RegCopyTreeW(hSrc, nullptr, hDst);
                }
                hDst.Close();
                hSrc.Close();
                RegDeleteTreeNative(HKEY_CLASSES_ROOT, normalPath);
            }
        }
        return true;
    } else {
        if (RegKeyExists(HKEY_CLASSES_ROOT, hiddenPath)) {
            ScopedHKey hSrc, hDst;
            if (RegOpenKeyExW(HKEY_CLASSES_ROOT, hiddenPath.c_str(), 0, KEY_READ, hSrc.put()) == ERROR_SUCCESS) {
                if (RegCreateKeyExW(HKEY_CLASSES_ROOT, normalPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, hDst.put(), nullptr) == ERROR_SUCCESS) {
                    RegCopyTreeW(hSrc, nullptr, hDst);
                }
                hDst.Close();
                hSrc.Close();
                RegDeleteTreeNative(HKEY_CLASSES_ROOT, hiddenPath);
            }
        }
        return true;
    }
}

bool ContextMenuManager::IsShellNewDisabled(const std::wstring& ext) {
    std::wstring hiddenPath = ext + L"\\ShellNew_Disabled";
    return RegKeyExists(HKEY_CLASSES_ROOT, hiddenPath);
}

bool ContextMenuManager::CreateShellNew(const std::wstring& ext, bool enable) {
    std::wstring path = ext + L"\\ShellNew";
    if (!enable) {
        return RegDeleteTreeNative(HKEY_CLASSES_ROOT, path);
    }
    return RegSetString(HKEY_CLASSES_ROOT, path, L"NullFile", L"");
}

bool ContextMenuManager::HasShellNew(const std::wstring& ext) {
    return RegKeyExists(HKEY_CLASSES_ROOT, ext + L"\\ShellNew");
}

std::wstring ContextMenuManager::GetCurrentExePath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

void ContextMenuManager::NotifyShellUpdate() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void ContextMenuManager::DetectState() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto checkState = [](const std::wstring& keyPath, bool& outEnabled, bool& outShift) {
        outEnabled = RegKeyExists(HKEY_CLASSES_ROOT, keyPath);
        if (outEnabled) {
            outShift = RegValueExists(HKEY_CLASSES_ROOT, keyPath, L"Extended");
        }
    };

    for (auto& it : m_items) {
        // --- Main 16 Tweaks ---
        if (it.id == "emptyFolderContent") {
            checkState(L"Directory\\shell\\EmptyFolder", it.enabled, it.shiftOnly);
        } else if (it.id == "symlinkCreate") {
            checkState(L"*\\shell\\CreateSymlink", it.enabled, it.shiftOnly);
        } else if (it.id == "takeOwnershipUser") {
            checkState(L"*\\shell\\TakeOwnershipUser", it.enabled, it.shiftOnly);
        } else if (it.id == "takeOwnershipAdmin") {
            checkState(L"*\\shell\\TakeOwnershipAdmin", it.enabled, it.shiftOnly);
        } else if (it.id == "blockNetAccess") {
            checkState(L"exefile\\shell\\BlockNetAccess", it.enabled, it.shiftOnly);
        } else if (it.id == "cmdPsAnywhere") {
            checkState(L"Directory\\Background\\shell\\OpenCmdPrompt", it.enabled, it.shiftOnly);
        } else if (it.id == "installCab") {
            checkState(L"CABFolder\\shell\\InstallCab", it.enabled, it.shiftOnly);
        } else if (it.id == "hashFile") {
            checkState(L"*\\shell\\FileHash", it.enabled, it.shiftOnly);
        } else if (it.id == "compactSeparate") {
            checkState(L"*\\shell\\CompactFile", it.enabled, it.shiftOnly);
        } else if (it.id == "copyImageBuffer") {
            checkState(L"SystemFileAssociations\\image\\shell\\CopyImageToBuffer", it.enabled, it.shiftOnly);
        } else if (it.id == "permanentDelete") {
            checkState(L"*\\shell\\PermanentDelete", it.enabled, it.shiftOnly);
        } else if (it.id == "cleanJunkContext" || it.id == "cleanContext") {
            checkState(L"Directory\\Background\\shell\\CleanJunkContext", it.enabled, it.shiftOnly);
        } else if (it.id == "taskMgrContext") {
            checkState(L"Directory\\Background\\shell\\TaskMgrContext", it.enabled, it.shiftOnly);
        }
        // --- Sub-Panel 1: Компьютер (19 items) ---
        else if (it.id == "comp-admin") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\AdminTools", it.enabled, it.shiftOnly);
        else if (it.id == "comp-devmgmt") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\DevMgr", it.enabled, it.shiftOnly);
        else if (it.id == "comp-control") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\ControlPanel", it.enabled, it.shiftOnly);
        else if (it.id == "comp-appwiz") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\AppWiz", it.enabled, it.shiftOnly);
        else if (it.id == "comp-gpedit") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\GPEdit", it.enabled, it.shiftOnly);
        else if (it.id == "comp-regedit") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\Regedit", it.enabled, it.shiftOnly);
        else if (it.id == "comp-services") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\Services", it.enabled, it.shiftOnly);
        else if (it.id == "comp-diskmgmt") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\DiskMgmt", it.enabled, it.shiftOnly);
        else if (it.id == "comp-msconfig") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\MSConfig", it.enabled, it.shiftOnly);
        else if (it.id == "comp-taskschd") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\TaskSchd", it.enabled, it.shiftOnly);
        else if (it.id == "comp-winver") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\WinVer", it.enabled, it.shiftOnly);
        else if (it.id == "comp-eventvwr") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\EventVwr", it.enabled, it.shiftOnly);
        else if (it.id == "comp-sysprops") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\SysProps", it.enabled, it.shiftOnly);
        else if (it.id == "comp-replace-props") {
            it.enabled = RegKeyExists(HKEY_CLASSES_ROOT, L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\properties\\command");
        }
        else if (it.id == "comp-hosts") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\HostsFile", it.enabled, it.shiftOnly);
        else if (it.id == "comp-netconnections") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\NetConnections", it.enabled, it.shiftOnly);
        else if (it.id == "comp-perfmon") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\ReliabilityMonitor", it.enabled, it.shiftOnly);
        else if (it.id == "comp-resmon") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\ResMon", it.enabled, it.shiftOnly);
        else if (it.id == "comp-godmode") checkState(L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\GodMode", it.enabled, it.shiftOnly);

        // --- Sub-Panel 2: Рабочий стол (Добавление - 16 items) ---
        else if (it.id == "desk-restart-explorer") checkState(L"DesktopBackground\\Shell\\RestartExplorer", it.enabled, it.shiftOnly);
        else if (it.id == "desk-refresh-icons") checkState(L"DesktopBackground\\Shell\\RefreshIconCache", it.enabled, it.shiftOnly);
        else if (it.id == "desk-compact-os") checkState(L"DesktopBackground\\Shell\\QueryCompactOS", it.enabled, it.shiftOnly);
        else if (it.id == "desk-power-schemes") checkState(L"DesktopBackground\\Shell\\PowerSchemes", it.enabled, it.shiftOnly);
        else if (it.id == "desk-safe-mode") checkState(L"DesktopBackground\\Shell\\SafeModeOpts", it.enabled, it.shiftOnly);
        else if (it.id == "desk-toggle-net") checkState(L"DesktopBackground\\Shell\\ToggleNetAdapter", it.enabled, it.shiftOnly);
        else if (it.id == "desk-toggle-hidden") checkState(L"DesktopBackground\\Shell\\ToggleHiddenFiles", it.enabled, it.shiftOnly);
        else if (it.id == "desk-toggle-ext") checkState(L"DesktopBackground\\Shell\\ToggleFileExt", it.enabled, it.shiftOnly);
        else if (it.id == "desk-safe-remove") checkState(L"DesktopBackground\\Shell\\SafeRemoveHardware", it.enabled, it.shiftOnly);
        else if (it.id == "desk-screen-clip") checkState(L"DesktopBackground\\Shell\\ScreenClip", it.enabled, it.shiftOnly);
        else if (it.id == "desk-runas-ti") checkState(L"DesktopBackground\\Shell\\RunAsTI", it.enabled, it.shiftOnly);
        else if (it.id == "desk-theme-toggle") checkState(L"DesktopBackground\\Shell\\ToggleTheme", it.enabled, it.shiftOnly);
        else if (it.id == "desk-display-switch") checkState(L"DesktopBackground\\Shell\\DisplaySwitch", it.enabled, it.shiftOnly);
        else if (it.id == "desk-screen-off") checkState(L"DesktopBackground\\Shell\\TurnOffScreen", it.enabled, it.shiftOnly);
        else if (it.id == "desk-power-menu") checkState(L"DesktopBackground\\Shell\\ClassicShutdown", it.enabled, it.shiftOnly);
        else if (it.id == "desk-classic-personalize") checkState(L"DesktopBackground\\Shell\\ClassicPersonalize", it.enabled, it.shiftOnly);

        // --- Sub-Panel 3: Рабочий стол (Удаление - 4 items) ---
        else if (it.id == "del-amd") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"ACE") ||
                         IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"DesktopBackground\\shellex\\ContextMenuHandlers", L"ACE");
        }
        else if (it.id == "del-intel") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"igfxDTCM") ||
                         IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"DesktopBackground\\shellex\\ContextMenuHandlers", L"igfxDTCM");
        }
        else if (it.id == "del-nvidia") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"NvCplDesktopContext") ||
                         IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"DesktopBackground\\shellex\\ContextMenuHandlers", L"NvCplDesktopContext");
        }
        else if (it.id == "del-vs") {
            it.enabled = RegKeyExists(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell\\-AnyCode") ||
                         IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"Open With Visual Studio");
        }

        // --- Sub-Panel 4: Меню Создать (11 items) ---
        else if (it.id == "new-del-bmp") it.enabled = IsShellNewDisabled(L".bmp");
        else if (it.id == "new-del-contact") it.enabled = IsShellNewDisabled(L".contact");
        else if (it.id == "new-del-rtf") it.enabled = IsShellNewDisabled(L".rtf");
        else if (it.id == "new-del-txt") it.enabled = IsShellNewDisabled(L".txt");
        else if (it.id == "new-del-zip") it.enabled = IsShellNewDisabled(L".zip");
        else if (it.id == "new-del-briefcase") {
            // Briefcase is checked by default on modern Windows
            it.enabled = IsShellNewDisabled(L".Briefcase") || !HasShellNew(L".Briefcase");
        }
        else if (it.id == "new-add-cmd") it.enabled = HasShellNew(L".cmd");
        else if (it.id == "new-add-reg") it.enabled = HasShellNew(L".reg");
        else if (it.id == "new-add-vbs") it.enabled = HasShellNew(L".vbs");
        else if (it.id == "new-add-bat") it.enabled = HasShellNew(L".bat");
        else if (it.id == "new-add-ps1") it.enabled = HasShellNew(L".ps1");

        // --- Sub-Panel 5: Удаление мусора (13 items) ---
        else if (it.id == "junk-previous-versions") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shellex\\ContextMenuHandlers", L"{596AB062-B4D3-4215-AB88-8C197CEEB8E6}");
        }
        else if (it.id == "junk-paint-3d") {
            it.enabled = HasProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"SystemFileAssociations\\image\\shell\\Edit with Paint 3D");
        }
        else if (it.id == "junk-pin-start") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", L"{470C0EBD-DE5C-4ad9-9232-31BF60BE5046}") ||
                         IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"Folder\\shellex\\ContextMenuHandlers", L"PintoStartScreen");
        }
        else if (it.id == "junk-pin-quickaccess") {
            it.enabled = HasProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Folder\\shell\\pintohome");
        }
        else if (it.id == "junk-find-music") {
            // Checked by default in modern Windows
            it.enabled = HasProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Directory\\shell\\findmusic") ||
                         !RegKeyExists(HKEY_CLASSES_ROOT, L"Directory\\shell\\findmusic");
        }
        else if (it.id == "junk-share") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", L"ModernSharing");
        }
        else if (it.id == "junk-cast") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", L"PlayTo");
        }
        else if (it.id == "junk-library") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"Folder\\ShellEx\\ContextMenuHandlers", L"Library Location");
        }
        else if (it.id == "junk-map-drive") {
            it.enabled = HasProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\MapNetworkDrive");
        }
        else if (it.id == "junk-extract-all") {
            it.enabled = IsShellexHandlerHidden(HKEY_CLASSES_ROOT, L"CompressedFolder\\ShellEx\\ContextMenuHandlers", L"{b8cdffb0-7ba1-11d0-b4e6-0000f800e73e}");
        }
        else if (it.id == "junk-add-favorites") {
            it.enabled = HasProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"*\\shell\\pintohomefile");
        }
        else if (it.id == "junk-bitlocker") {
            it.enabled = HasProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Drive\\shell\\encrypt-bde");
        }
        else if (it.id == "junk-manage") {
            it.enabled = HasProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\Manage");
        }
    }

    // Automatically remove deprecated upload verbs from Windows Registry if previously created
    RemoveShellVerb(HKEY_CLASSES_ROOT, L"*\\shell\\UploadToUploadEE");
    RemoveShellVerb(HKEY_CLASSES_ROOT, L"SystemFileAssociations\\image\\shell\\UploadToImgur");
    RemoveShellVerb(HKEY_CLASSES_ROOT, L"SystemFileAssociations\\image\\shell\\UploadToImgbb");
}

nlohmann::json ContextMenuManager::GetStateJson() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& it : m_items) {
        nlohmann::json obj;
        obj["id"] = it.id;
        obj["title"] = it.title;
        obj["description"] = it.description;
        obj["enabled"] = it.enabled;
        obj["shiftOnly"] = it.shiftOnly;
        arr.push_back(obj);
    }
    return arr;
}

bool ContextMenuManager::HasTweak(const std::string& id) const {
    std::string cleanId = id;
    if (cleanId.rfind("ctx-", 0) == 0) cleanId = cleanId.substr(4);
    if (cleanId == "cleanContext") cleanId = "cleanJunkContext";

    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& it : m_items) {
        if (it.id == cleanId) return true;
    }
    return false;
}

bool ContextMenuManager::ToggleTweak(const std::string& id, bool enabled, std::string& outMsg) {
    return SetTweak(id, enabled, outMsg);
}

bool ContextMenuManager::SetShiftOnly(const std::string& id, bool shiftOnly, std::string& outMsg) {
    std::string cleanId = id;
    if (cleanId.rfind("ctx-", 0) == 0) cleanId = cleanId.substr(4);
    if (cleanId == "cleanContext") cleanId = "cleanJunkContext";

    bool found = false;
    bool isEnabled = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& it : m_items) {
            if (it.id == cleanId) {
                it.shiftOnly = shiftOnly;
                isEnabled = it.enabled;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        outMsg = "Твик контекстного меню не найден: " + id;
        return false;
    }

    if (isEnabled) {
        return SetTweak(cleanId, true, outMsg);
    }

    outMsg = shiftOnly ? "Режим Shift+ПКМ включен" : "Режим Shift+ПКМ выключен";
    return true;
}

bool ContextMenuManager::SetTweak(const std::string& id, bool enabled, std::string& outMsg) {
    std::string cleanId = id;
    if (cleanId.rfind("ctx-", 0) == 0) cleanId = cleanId.substr(4);
    if (cleanId == "cleanContext") cleanId = "cleanJunkContext";

    bool shiftOnly = false;
    std::string tweakTitle;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& it : m_items) {
            if (it.id == cleanId) {
                shiftOnly = it.shiftOnly;
                tweakTitle = it.title;
                break;
            }
        }
    }

    bool ok = false;
    // --- Main 16 Tweaks ---
    if (cleanId == "emptyFolderContent") ok = ApplyEmptyFolder(enabled, shiftOnly);
    else if (cleanId == "symlinkCreate") ok = ApplySymlink(enabled, shiftOnly);
    else if (cleanId == "takeOwnershipUser") ok = ApplyTakeOwnershipUser(enabled, shiftOnly);
    else if (cleanId == "takeOwnershipAdmin") ok = ApplyTakeOwnershipAdmin(enabled, shiftOnly);
    else if (cleanId == "blockNetAccess") ok = ApplyBlockNetAccess(enabled, shiftOnly);
    else if (cleanId == "cmdPsAnywhere") ok = ApplyCmdPsAnywhere(enabled, shiftOnly);
    else if (cleanId == "installCab") ok = ApplyInstallCab(enabled, shiftOnly);
    else if (cleanId == "hashFile") ok = ApplyHashFile(enabled, shiftOnly);
    else if (cleanId == "compactSeparate") ok = ApplyCompactSeparate(enabled, shiftOnly);
    else if (cleanId == "copyImageBuffer") ok = ApplyCopyImageBuffer(enabled, shiftOnly);
    else if (cleanId == "permanentDelete") ok = ApplyPermanentDelete(enabled, shiftOnly);
    else if (cleanId == "cleanJunkContext") ok = ApplyCleanJunkContext(enabled, shiftOnly);
    else if (cleanId == "taskMgrContext") ok = ApplyTaskMgrContext(enabled, shiftOnly);

    // --- Sub-Panel 1: Компьютер (19 items) ---
    else if (cleanId == "comp-admin") ok = ApplyCompAdmin(enabled);
    else if (cleanId == "comp-devmgmt") ok = ApplyCompDevmgmt(enabled);
    else if (cleanId == "comp-control") ok = ApplyCompControl(enabled);
    else if (cleanId == "comp-appwiz") ok = ApplyCompAppwiz(enabled);
    else if (cleanId == "comp-gpedit") ok = ApplyCompGpedit(enabled);
    else if (cleanId == "comp-regedit") ok = ApplyCompRegedit(enabled);
    else if (cleanId == "comp-services") ok = ApplyCompServices(enabled);
    else if (cleanId == "comp-diskmgmt") ok = ApplyCompDiskmgmt(enabled);
    else if (cleanId == "comp-msconfig") ok = ApplyCompMsconfig(enabled);
    else if (cleanId == "comp-taskschd") ok = ApplyCompTaskschd(enabled);
    else if (cleanId == "comp-winver") ok = ApplyCompWinver(enabled);
    else if (cleanId == "comp-eventvwr") ok = ApplyCompEventvwr(enabled);
    else if (cleanId == "comp-sysprops") ok = ApplyCompSysprops(enabled);
    else if (cleanId == "comp-replace-props") ok = ApplyCompReplaceProps(enabled);
    else if (cleanId == "comp-hosts") ok = ApplyCompHosts(enabled);
    else if (cleanId == "comp-netconnections") ok = ApplyCompNetconnections(enabled);
    else if (cleanId == "comp-perfmon") ok = ApplyCompPerfmon(enabled);
    else if (cleanId == "comp-resmon") ok = ApplyCompResmon(enabled);
    else if (cleanId == "comp-godmode") ok = ApplyCompGodmode(enabled, shiftOnly);

    // --- Sub-Panel 2: Рабочий стол (Добавление - 16 items) ---
    else if (cleanId == "desk-restart-explorer") ok = ApplyDeskRestartExplorer(enabled);
    else if (cleanId == "desk-refresh-icons") ok = ApplyDeskRefreshIcons(enabled);
    else if (cleanId == "desk-compact-os") ok = ApplyDeskCompactOs(enabled, shiftOnly);
    else if (cleanId == "desk-power-schemes") ok = ApplyDeskPowerSchemes(enabled, shiftOnly);
    else if (cleanId == "desk-safe-mode") ok = ApplyDeskSafeMode(enabled, shiftOnly);
    else if (cleanId == "desk-toggle-net") ok = ApplyDeskToggleNet(enabled, shiftOnly);
    else if (cleanId == "desk-toggle-hidden") ok = ApplyDeskToggleHidden(enabled, shiftOnly);
    else if (cleanId == "desk-toggle-ext") ok = ApplyDeskToggleExt(enabled, shiftOnly);
    else if (cleanId == "desk-safe-remove") ok = ApplyDeskSafeRemove(enabled, shiftOnly);
    else if (cleanId == "desk-screen-clip") ok = ApplyDeskScreenClip(enabled, shiftOnly);
    else if (cleanId == "desk-runas-ti") ok = ApplyDeskRunAsTi(enabled, shiftOnly);
    else if (cleanId == "desk-theme-toggle") ok = ApplyDeskThemeToggle(enabled);
    else if (cleanId == "desk-display-switch") ok = ApplyDeskDisplaySwitch(enabled);
    else if (cleanId == "desk-screen-off") ok = ApplyDeskScreenOff(enabled);
    else if (cleanId == "desk-power-menu") ok = ApplyDeskPowerMenu(enabled);
    else if (cleanId == "desk-classic-personalize") ok = ApplyDeskClassicPersonalize(enabled);

    // --- Sub-Panel 3: Рабочий стол (Удаление - 4 items) ---
    else if (cleanId == "del-amd") ok = ApplyDelAmd(enabled);
    else if (cleanId == "del-intel") ok = ApplyDelIntel(enabled);
    else if (cleanId == "del-nvidia") ok = ApplyDelNvidia(enabled);
    else if (cleanId == "del-vs") ok = ApplyDelVs(enabled);

    // --- Sub-Panel 4: Меню Создать (11 items) ---
    else if (cleanId == "new-del-bmp") ok = ApplyNewDelBmp(enabled);
    else if (cleanId == "new-del-contact") ok = ApplyNewDelContact(enabled);
    else if (cleanId == "new-del-rtf") ok = ApplyNewDelRtf(enabled);
    else if (cleanId == "new-del-txt") ok = ApplyNewDelTxt(enabled);
    else if (cleanId == "new-del-zip") ok = ApplyNewDelZip(enabled);
    else if (cleanId == "new-del-briefcase") ok = ApplyNewDelBriefcase(enabled);
    else if (cleanId == "new-add-cmd") ok = ApplyNewAddCmd(enabled);
    else if (cleanId == "new-add-reg") ok = ApplyNewAddReg(enabled);
    else if (cleanId == "new-add-vbs") ok = ApplyNewAddVbs(enabled);
    else if (cleanId == "new-add-bat") ok = ApplyNewAddBat(enabled);
    else if (cleanId == "new-add-ps1") ok = ApplyNewAddPs1(enabled);

    // --- Sub-Panel 5: Удаление мусора (13 items) ---
    else if (cleanId == "junk-previous-versions") ok = ApplyJunkPreviousVersions(enabled);
    else if (cleanId == "junk-paint-3d") ok = ApplyJunkPaint3d(enabled);
    else if (cleanId == "junk-pin-start") ok = ApplyJunkPinStart(enabled);
    else if (cleanId == "junk-pin-quickaccess") ok = ApplyJunkPinQuickaccess(enabled);
    else if (cleanId == "junk-find-music") ok = ApplyJunkFindMusic(enabled);
    else if (cleanId == "junk-share") ok = ApplyJunkShare(enabled);
    else if (cleanId == "junk-cast") ok = ApplyJunkCast(enabled);
    else if (cleanId == "junk-library") ok = ApplyJunkLibrary(enabled);
    else if (cleanId == "junk-map-drive") ok = ApplyJunkMapDrive(enabled);
    else if (cleanId == "junk-extract-all") ok = ApplyJunkExtractAll(enabled);
    else if (cleanId == "junk-add-favorites") ok = ApplyJunkAddFavorites(enabled);
    else if (cleanId == "junk-bitlocker") ok = ApplyJunkBitlocker(enabled);
    else if (cleanId == "junk-manage") ok = ApplyJunkManage(enabled);

    else {
        outMsg = "Неизвестный параметр меню: " + id;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& it : m_items) {
            if (it.id == cleanId) {
                it.enabled = enabled;
                break;
            }
        }
    }

    NotifyShellUpdate();

    outMsg = enabled ? (tweakTitle + " — успешно применено") : (tweakTitle + " — отключено");
    DebugLogger::Get().LogTweak("ContextMenu_" + cleanId, enabled, outMsg);
    return ok;
}

bool ContextMenuManager::ApplyAll(bool enable, std::string& outMsg) {
    // Apply all 13 main tweaks
    const std::vector<std::string> mainIds = {
        "emptyFolderContent", "symlinkCreate", "takeOwnershipUser", "takeOwnershipAdmin",
        "blockNetAccess", "cmdPsAnywhere", "installCab", "hashFile",
        "compactSeparate", "copyImageBuffer", "permanentDelete", "cleanJunkContext", "taskMgrContext"
    };

    int successCount = 0;
    std::string singleMsg;
    for (const auto& id : mainIds) {
        if (SetTweak(id, enable, singleMsg)) {
            successCount++;
        }
    }

    NotifyShellUpdate();
    outMsg = enable ? "Все пункты контекстного меню активированы (" + std::to_string(successCount) + "/13)"
                    : "Все пункты контекстного меню удалены (" + std::to_string(successCount) + "/13)";
    return (successCount > 0);
}

bool ContextMenuManager::ApplyGroup(const std::string& group, bool enable, std::string& outMsg) {
    std::string prefix;
    if (group == "comp") prefix = "comp-";
    else if (group == "desk-add") prefix = "desk-";
    else if (group == "desk-del") prefix = "del-";
    else if (group == "new") prefix = "new-";
    else if (group == "junk") prefix = "junk-";
    else {
        outMsg = "Неизвестная группа: " + group;
        return false;
    }

    std::vector<std::string> targetIds;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& it : m_items) {
            if (it.id.rfind(prefix, 0) == 0) {
                targetIds.push_back(it.id);
            }
        }
    }

    int count = 0;
    std::string dummy;
    for (const auto& id : targetIds) {
        if (SetTweak(id, enable, dummy)) {
            count++;
        }
    }

    NotifyShellUpdate();
    outMsg = enable ? "Все пункты группы успешно применены (" + std::to_string(count) + "/" + std::to_string(targetIds.size()) + ")"
                    : "Все пункты группы отключены (" + std::to_string(count) + "/" + std::to_string(targetIds.size()) + ")";
    return (count > 0);
}

// ============================================================================
// MAIN 16 TWEAKS IMPLEMENTATION
// ============================================================================

// 1. Удалить содержимое папки
bool ContextMenuManager::ApplyEmptyFolder(bool enable, bool shiftOnly) {
    const std::wstring pathDir = L"Directory\\shell\\EmptyFolder";
    const std::wstring pathBg = L"Directory\\Background\\shell\\EmptyFolder";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, pathDir);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, pathBg);
    }

    std::wstring cmdDir = L"cmd.exe /c \"cd /d \\\"%1\\\" && del /f /q /s *.* >nul 2>&1 && for /d %p in (*) do rmdir /s /q \\\"%p\\\" >nul 2>&1\"";
    std::wstring cmdBg = L"cmd.exe /c \"cd /d \\\"%V\\\" && del /f /q /s *.* >nul 2>&1 && for /d %p in (*) do rmdir /s /q \\\"%p\\\" >nul 2>&1\"";
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, pathDir, L"Удалить содержимое папки", cmdDir, L"imageres.dll,-5306", shiftOnly);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, pathBg, L"Удалить содержимое папки", cmdBg, L"imageres.dll,-5306", shiftOnly);
    return ok1 && ok2;
}

// 2. Создать символическую ссылку
bool ContextMenuManager::ApplySymlink(bool enable, bool shiftOnly) {
    const std::wstring pathFile = L"*\\shell\\CreateSymlink";
    const std::wstring pathDir = L"Directory\\shell\\CreateSymlink";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, pathFile);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, pathDir);
    }
    std::wstring cmdFile = L"cmd.exe /c for %i in (\"%1\") do mklink \"%~dpi%~ni - Symlink%~xi\" \"%~fi\"";
    std::wstring cmdDir = L"cmd.exe /c for %i in (\"%1\") do mklink /d \"%~dpi%~ni - Symlink\" \"%~fi\"";
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, pathFile, L"Создать символическую ссылку", cmdFile, L"imageres.dll,-1030", shiftOnly, true);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, pathDir, L"Создать символическую ссылку", cmdDir, L"imageres.dll,-1030", shiftOnly, true);
    return ok1 && ok2;
}

// 3. Стать владельцем и получить пользовательский доступ
bool ContextMenuManager::ApplyTakeOwnershipUser(bool enable, bool shiftOnly) {
    const std::wstring pathFile = L"*\\shell\\TakeOwnershipUser";
    const std::wstring pathDir = L"Directory\\shell\\TakeOwnershipUser";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, pathFile);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, pathDir);
    }
    std::wstring cmd = L"cmd.exe /c takeown /f \"%1\" /r /d y & icacls \"%1\" /grant %username%:F /t /c /l /q";
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, pathFile, L"Стать владельцем (Пользователь)", cmd, L"imageres.dll,-78", shiftOnly, true);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, pathDir, L"Стать владельцем (Пользователь)", cmd, L"imageres.dll,-78", shiftOnly, true);
    return ok1 && ok2;
}

// 4. Стать владельцем и получить доступ администратора
bool ContextMenuManager::ApplyTakeOwnershipAdmin(bool enable, bool shiftOnly) {
    const std::wstring pathFile = L"*\\shell\\TakeOwnershipAdmin";
    const std::wstring pathDir = L"Directory\\shell\\TakeOwnershipAdmin";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, pathFile);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, pathDir);
    }
    std::wstring cmd = L"cmd.exe /c takeown /f \"%1\" /r /d y & icacls \"%1\" /grant *S-1-5-32-544:F /t /c /l /q";
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, pathFile, L"Стать владельцем и получить доступ администратора", cmd, L"imageres.dll,-1029", shiftOnly, true);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, pathDir, L"Стать владельцем и получить доступ администратора", cmd, L"imageres.dll,-1029", shiftOnly, true);
    return ok1 && ok2;
}

// 5. Запретить/Разрешить приложению доступ в Интернет
bool ContextMenuManager::ApplyBlockNetAccess(bool enable, bool shiftOnly) {
    const std::wstring blockPath = L"exefile\\shell\\BlockNetAccess";
    const std::wstring allowPath = L"exefile\\shell\\AllowNetAccess";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, blockPath);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, allowPath);
    }
    std::wstring blockCmd = L"cmd.exe /c for %a in (\"%1\") do (netsh advfirewall firewall add rule name=\"Block_%~nxa\" dir=out action=block program=\"%~fa\" enable=yes & netsh advfirewall firewall add rule name=\"Block_%~nxa\" dir=in action=block program=\"%~fa\" enable=yes)";
    std::wstring allowCmd = L"cmd.exe /c for %a in (\"%1\") do netsh advfirewall firewall delete rule name=\"Block_%~nxa\"";

    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, blockPath, L"Запретить приложению доступ в Интернет", blockCmd, L"imageres.dll,-102", shiftOnly, true);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, allowPath, L"Разрешить приложению доступ в Интернет", allowCmd, L"imageres.dll,-101", shiftOnly, true);
    return ok1 && ok2;
}

// 6. Открыть окна CMD и PowerShell из любого места
bool ContextMenuManager::ApplyCmdPsAnywhere(bool enable, bool shiftOnly) {
    const std::vector<std::wstring> dirs = {
        L"Directory\\Background\\shell\\OpenCmdPrompt",
        L"Directory\\Background\\shell\\OpenPowerShell",
        L"Directory\\shell\\OpenCmdPrompt",
        L"Directory\\shell\\OpenPowerShell",
        L"Drive\\shell\\OpenCmdPrompt",
        L"Drive\\shell\\OpenPowerShell"
    };

    if (!enable) {
        for (const auto& d : dirs) RemoveShellVerb(HKEY_CLASSES_ROOT, d);
        return true;
    }

    SetShellVerb(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell\\OpenCmdPrompt", L"Открыть окно команд (CMD) здесь", L"cmd.exe /s /k pushd \"%V\"", L"cmd.exe", shiftOnly);
    SetShellVerb(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell\\OpenPowerShell", L"Открыть окно PowerShell здесь", L"powershell.exe -NoExit -NoProfile -Command \"Set-Location -LiteralPath '%V'\"", L"powershell.exe", shiftOnly);
    SetShellVerb(HKEY_CLASSES_ROOT, L"Directory\\shell\\OpenCmdPrompt", L"Открыть окно команд (CMD) здесь", L"cmd.exe /s /k pushd \"%1\"", L"cmd.exe", shiftOnly);
    SetShellVerb(HKEY_CLASSES_ROOT, L"Directory\\shell\\OpenPowerShell", L"Открыть окно PowerShell здесь", L"powershell.exe -NoExit -NoProfile -Command \"Set-Location -LiteralPath '%1'\"", L"powershell.exe", shiftOnly);
    SetShellVerb(HKEY_CLASSES_ROOT, L"Drive\\shell\\OpenCmdPrompt", L"Открыть окно команд (CMD) здесь", L"cmd.exe /s /k pushd \"%1\"", L"cmd.exe", shiftOnly);
    SetShellVerb(HKEY_CLASSES_ROOT, L"Drive\\shell\\OpenPowerShell", L"Открыть окно PowerShell здесь", L"powershell.exe -NoExit -NoProfile -Command \"Set-Location -LiteralPath '%1'\"", L"powershell.exe", shiftOnly);
    return true;
}

// 7. Установить CAB
bool ContextMenuManager::ApplyInstallCab(bool enable, bool shiftOnly) {
    const std::wstring path1 = L"CABFolder\\shell\\InstallCab";
    const std::wstring path2 = L"SystemFileAssociations\\.cab\\shell\\InstallCab";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, path1);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, path2);
    }
    std::wstring cmd = L"cmd.exe /k dism.exe /online /add-package /packagepath:\"%1\"";
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, path1, L"Установить CAB-пакет", cmd, L"imageres.dll,-78", shiftOnly, true);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, path2, L"Установить CAB-пакет", cmd, L"imageres.dll,-78", shiftOnly, true);
    return ok1 && ok2;
}

// 8. Хеш-суммы файла
bool ContextMenuManager::ApplyHashFile(bool enable, bool shiftOnly) {
    const std::wstring path = L"*\\shell\\FileHash";
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, path);

    std::wstring cmd = L"cmd.exe /k \"echo ================= Хеш-суммы файла ================= & echo. & echo [SHA256] & certutil -hashfile \"%1\" SHA256 & echo. & echo [MD5] & certutil -hashfile \"%1\" MD5 & echo. & echo [SHA1] & certutil -hashfile \"%1\" SHA1 & echo. & echo ==================================================== & pause\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, path, L"Хеш-суммы файла (SHA256 / MD5 / SHA1)", cmd, L"imageres.dll,-5308", shiftOnly);
}

// 9. Сжать отдельно файл/директорию
bool ContextMenuManager::ApplyCompactSeparate(bool enable, bool shiftOnly) {
    const std::wstring pathFile = L"*\\shell\\CompactFile";
    const std::wstring pathDir = L"Directory\\shell\\CompactFolder";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, pathFile);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, pathDir);
    }
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, pathFile, L"Сжать отдельно файл (CompactOS)", L"cmd.exe /c compact.exe /c /a /i /exe:lzx \"%1\" & pause", L"imageres.dll,-165", shiftOnly);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, pathDir, L"Сжать отдельно директорию (CompactOS)", L"cmd.exe /c compact.exe /c /s /a /i /exe:lzx \"%1\\*\" & pause", L"imageres.dll,-165", shiftOnly);
    return ok1 && ok2;
}

// 13. Копировать в буфер на изображениях
bool ContextMenuManager::ApplyCopyImageBuffer(bool enable, bool shiftOnly) {
    const std::wstring path = L"SystemFileAssociations\\image\\shell\\CopyImageToBuffer";
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, path);

    std::wstring cmd = L"powershell.exe -sta -NoProfile -WindowStyle Hidden -Command \"$f='%1'; if(Test-Path -LiteralPath $f){ try { Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing; $img=[System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $f)); [System.Windows.Forms.Clipboard]::SetDataObject($img, $true); $img.Dispose() } catch {} }\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, path, L"Копировать в буфер на изображениях", cmd, L"shell32.dll,-16769", shiftOnly);
}

// 14. Удалить окончательно
bool ContextMenuManager::ApplyPermanentDelete(bool enable, bool shiftOnly) {
    const std::wstring pathFile = L"*\\shell\\PermanentDelete";
    const std::wstring pathDir = L"Directory\\shell\\PermanentDelete";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, pathFile);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, pathDir);
    }
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, pathFile, L"Удалить окончательно", L"cmd.exe /c del /f /q /a \"%1\"", L"shell32.dll,-240", shiftOnly, true);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, pathDir, L"Удалить окончательно", L"cmd.exe /c rd /s /q \"%1\"", L"shell32.dll,-240", shiftOnly, true);
    return ok1 && ok2;
}

// 15. Очистка системы, папок и реестра от мусора
bool ContextMenuManager::ApplyCleanJunkContext(bool enable, bool shiftOnly) {
    const std::wstring pathBg = L"Directory\\Background\\shell\\CleanJunkContext";
    const std::wstring pathDir = L"Directory\\shell\\CleanJunkContext";
    if (!enable) {
        RemoveShellVerb(HKEY_CLASSES_ROOT, pathBg);
        return RemoveShellVerb(HKEY_CLASSES_ROOT, pathDir);
    }
    std::wstring exe = GetCurrentExePath();
    std::wstring cmd = L"\"" + exe + L"\" --clean-junk";
    bool ok1 = SetShellVerb(HKEY_CLASSES_ROOT, pathBg, L"Очистка системы, папок и реестра от мусора", cmd, exe, shiftOnly, true);
    bool ok2 = SetShellVerb(HKEY_CLASSES_ROOT, pathDir, L"Очистка системы, папок и реестра от мусора", cmd, exe, shiftOnly, true);
    return ok1 && ok2;
}

// 16. Диспетчер задач в контекстное меню
bool ContextMenuManager::ApplyTaskMgrContext(bool enable, bool shiftOnly) {
    const std::wstring path = L"Directory\\Background\\shell\\TaskMgrContext";
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, path);

    return SetShellVerb(HKEY_CLASSES_ROOT, path, L"Диспетчер задач", L"taskmgr.exe", L"taskmgr.exe,0", shiftOnly);
}

// ============================================================================
// SUB-PANEL 1: THIS PC (Компьютер - 19 items)
// ============================================================================
static const std::wstring kCompShell = L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\";

bool ContextMenuManager::ApplyCompAdmin(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"AdminTools");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"AdminTools", L"Администрирование", L"control.exe admintools", L"imageres.dll,-114");
}

bool ContextMenuManager::ApplyCompDevmgmt(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"DevMgr");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"DevMgr", L"Диспетчер устройств", L"mmc.exe devmgmt.msc", L"devmgmt.msc,-4");
}

bool ContextMenuManager::ApplyCompControl(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"ControlPanel");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"ControlPanel", L"Панель управления", L"control.exe", L"control.exe");
}

bool ContextMenuManager::ApplyCompAppwiz(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"AppWiz");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"AppWiz", L"Программы и компоненты", L"control.exe appwiz.cpl", L"appwiz.cpl,-1");
}

bool ContextMenuManager::ApplyCompGpedit(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"GPEdit");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"GPEdit", L"Редактор групповой политики", L"gpedit.msc", L"gpedit.msc,-1");
}

bool ContextMenuManager::ApplyCompRegedit(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"Regedit");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"Regedit", L"Редактор реестра", L"regedit.exe", L"regedit.exe,-16", false, true);
}

bool ContextMenuManager::ApplyCompServices(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"Services");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"Services", L"Службы", L"services.msc", L"filemgmt.dll,-220");
}

bool ContextMenuManager::ApplyCompDiskmgmt(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"DiskMgmt");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"DiskMgmt", L"Управление дисками", L"diskmgmt.msc", L"dmdskres.dll,-344");
}

bool ContextMenuManager::ApplyCompMsconfig(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"MSConfig");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"MSConfig", L"Конфигурация системы", L"msconfig.exe", L"msconfig.exe");
}

bool ContextMenuManager::ApplyCompTaskschd(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"TaskSchd");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"TaskSchd", L"Планировщик заданий", L"taskschd.msc", L"miguiresource.dll,-101");
}

bool ContextMenuManager::ApplyCompWinver(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"WinVer");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"WinVer", L"Версия Windows", L"winver.exe", L"shell32.dll,-290");
}

bool ContextMenuManager::ApplyCompEventvwr(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"EventVwr");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"EventVwr", L"Просмотр событий", L"eventvwr.msc", L"eventvwr.msc");
}

bool ContextMenuManager::ApplyCompSysprops(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"SysProps");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"SysProps", L"Свойства системы (Классическое)", L"control.exe sysdm.cpl", L"sysdm.cpl");
}

bool ContextMenuManager::ApplyCompReplaceProps(bool enable) {
    const std::wstring keyPath = kCompShell + L"properties\\command";
    if (!enable) {
        return RegDeleteTreeNative(HKEY_CLASSES_ROOT, kCompShell + L"properties");
    }
    return RegSetString(HKEY_CLASSES_ROOT, keyPath, L"", L"control.exe sysdm.cpl");
}

bool ContextMenuManager::ApplyCompHosts(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"HostsFile");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"HostsFile", L"Редактирование файла Hosts", L"notepad.exe %windir%\\system32\\drivers\\etc\\hosts", L"notepad.exe", false, true);
}

bool ContextMenuManager::ApplyCompNetconnections(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"NetConnections");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"NetConnections", L"Сетевые подключения", L"control.exe ncpa.cpl", L"ncpa.cpl");
}

bool ContextMenuManager::ApplyCompPerfmon(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"ReliabilityMonitor");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"ReliabilityMonitor", L"Монитор стабильности системы", L"perfmon.exe /rel", L"perfmon.exe");
}

bool ContextMenuManager::ApplyCompResmon(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"ResMon");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"ResMon", L"Монитор ресурсов системы", L"resmon.exe", L"resmon.exe");
}

bool ContextMenuManager::ApplyCompGodmode(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"GodMode");
    return SetShellVerb(HKEY_CLASSES_ROOT, kCompShell + L"GodMode", L"Все задачи", L"explorer.exe shell:::{ED7BA470-8E54-465E-825C-99712043E01C}", L"imageres.dll,-42", shiftOnly);
}

// ============================================================================
// SUB-PANEL 2: DESKTOP ADD (Рабочий стол Добавление - 16 items)
// ============================================================================
static const std::wstring kDeskShell = L"DesktopBackground\\Shell\\";

bool ContextMenuManager::ApplyDeskRestartExplorer(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"RestartExplorer");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"RestartExplorer", L"Перезапуск проводника", L"cmd.exe /c taskkill /f /im explorer.exe & start explorer.exe", L"imageres.dll,-229");
}

bool ContextMenuManager::ApplyDeskRefreshIcons(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"RefreshIconCache");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"RefreshIconCache", L"Обновить кэш иконок", L"cmd.exe /c ie4uinit.exe -show", L"shell32.dll,-238");
}

bool ContextMenuManager::ApplyDeskCompactOs(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"QueryCompactOS");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"QueryCompactOS", L"Проверка наличия сжатия ОС", L"cmd.exe /k compact.exe /CompactOS:query", L"imageres.dll,-165", shiftOnly);
}

bool ContextMenuManager::ApplyDeskPowerSchemes(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"PowerSchemes");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"PowerSchemes", L"Схемы управления питанием", L"control.exe powercfg.cpl", L"powercpl.dll", shiftOnly);
}

bool ContextMenuManager::ApplyDeskSafeMode(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"SafeModeOpts");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"SafeModeOpts", L"Безопасный режим", L"shutdown.exe /r /o /t 0", L"imageres.dll,-103", shiftOnly, true);
}

bool ContextMenuManager::ApplyDeskToggleNet(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleNetAdapter");
    std::wstring cmd = L"powershell.exe -WindowStyle Hidden -Command \"Get-NetAdapter | ForEach-Object { if ($_.Status -eq 'Up') { Disable-NetAdapter -Name $_.Name -Confirm:$false } else { Enable-NetAdapter -Name $_.Name -Confirm:$false } }\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleNetAdapter", L"Отключить/Включить интернет", cmd, L"netcenter.dll", shiftOnly, true);
}

bool ContextMenuManager::ApplyDeskToggleHidden(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleHiddenFiles");
    std::wstring cmd = L"powershell.exe -WindowStyle Hidden -Command \"$k='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced'; $v=(Get-ItemProperty $k).Hidden; if($v -eq 1){Set-ItemProperty $k Hidden 2}else{Set-ItemProperty $k Hidden 1}; (New-Object -ComObject Shell.Application).Windows() | ForEach-Object { $_.Refresh() }\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleHiddenFiles", L"Показать/Скрыть скрытые файлы и папки", cmd, L"shell32.dll,-152", shiftOnly);
}

bool ContextMenuManager::ApplyDeskToggleExt(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleFileExt");
    std::wstring cmd = L"powershell.exe -WindowStyle Hidden -Command \"$k='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced'; $v=(Get-ItemProperty $k).HideFileExt; if($v -eq 1){Set-ItemProperty $k HideFileExt 0}else{Set-ItemProperty $k HideFileExt 1}; (New-Object -ComObject Shell.Application).Windows() | ForEach-Object { $_.Refresh() }\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleFileExt", L"Показать/Скрыть расширения файлов", cmd, L"shell32.dll,-152", shiftOnly);
}

bool ContextMenuManager::ApplyDeskSafeRemove(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"SafeRemoveHardware");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"SafeRemoveHardware", L"Безопасное извлечение", L"rundll32.exe shell32.dll,Control_RunDLL hotplug.dll", L"hotplug.dll,-100", shiftOnly);
}

bool ContextMenuManager::ApplyDeskScreenClip(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ScreenClip");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ScreenClip", L"Набросок на фрагменте экрана", L"cmd.exe /c start ms-screenclip:", L"imageres.dll,-102", shiftOnly);
}

bool ContextMenuManager::ApplyDeskRunAsTi(bool enable, bool shiftOnly) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"RunAsTI");
    std::wstring cmd = L"powershell.exe -WindowStyle Hidden -Command \"Start-Process cmd.exe -Verb RunAs\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"RunAsTI", L"Запускать от имени TI", cmd, L"imageres.dll,-78", shiftOnly, true);
}

bool ContextMenuManager::ApplyDeskThemeToggle(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleTheme");
    std::wstring cmd = L"powershell.exe -WindowStyle Hidden -Command \"$p='HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize'; $v=(Get-ItemProperty $p).AppsUseLightTheme; if($v -eq 1){Set-ItemProperty $p SystemUsesLightTheme 0; Set-ItemProperty $p AppsUseLightTheme 0}else{Set-ItemProperty $p SystemUsesLightTheme 1; Set-ItemProperty $p AppsUseLightTheme 1}\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ToggleTheme", L"Тема День/Ночь", cmd, L"themecpl.dll");
}

bool ContextMenuManager::ApplyDeskDisplaySwitch(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"DisplaySwitch");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"DisplaySwitch", L"Переключение экранов с двумя мониторами", L"DisplaySwitch.exe", L"DisplaySwitch.exe");
}

bool ContextMenuManager::ApplyDeskScreenOff(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"TurnOffScreen");
    std::wstring cmd = L"powershell.exe -WindowStyle Hidden -Command \"(Add-Type -MemberDefinition '[DllImport(\\\"user32.dll\\\")]public static extern int SendMessage(int hWnd,int hMsg,int wParam,int lParam);' -Name a -PassThru)::SendMessage(-1,0x0112,0xF170,2)\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"TurnOffScreen", L"Отключить экран", cmd, L"imageres.dll,-1006");
}

bool ContextMenuManager::ApplyDeskPowerMenu(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ClassicShutdown");
    std::wstring cmd = L"powershell.exe -WindowStyle Hidden -Command \"(New-Object -ComObject Shell.Application).ShutdownWindows()\"";
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ClassicShutdown", L"Выключить или перезагрузить компьютер", cmd, L"shell32.dll,-28");
}

bool ContextMenuManager::ApplyDeskClassicPersonalize(bool enable) {
    if (!enable) return RemoveShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ClassicPersonalize");
    return SetShellVerb(HKEY_CLASSES_ROOT, kDeskShell + L"ClassicPersonalize", L"Персонализация (Классическая)", L"control.exe /name Microsoft.Personalization", L"themecpl.dll");
}

// ============================================================================
// SUB-PANEL 3: DESKTOP DEL (Удаление пунктов рабочего стола - 4 items)
// ============================================================================
bool ContextMenuManager::ApplyDelAmd(bool enable) {
    bool ok1 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"ACE", enable);
    bool ok2 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"DesktopBackground\\shellex\\ContextMenuHandlers", L"ACE", enable);
    return ok1 || ok2;
}

bool ContextMenuManager::ApplyDelIntel(bool enable) {
    bool ok1 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"igfxDTCM", enable);
    bool ok2 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"DesktopBackground\\shellex\\ContextMenuHandlers", L"igfxDTCM", enable);
    ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"igfxcui", enable);
    ToggleShellexHandler(HKEY_CLASSES_ROOT, L"DesktopBackground\\shellex\\ContextMenuHandlers", L"igfxcui", enable);
    return ok1 || ok2;
}

bool ContextMenuManager::ApplyDelNvidia(bool enable) {
    bool ok1 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"NvCplDesktopContext", enable);
    bool ok2 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"DesktopBackground\\shellex\\ContextMenuHandlers", L"NvCplDesktopContext", enable);
    return ok1 || ok2;
}

bool ContextMenuManager::ApplyDelVs(bool enable) {
    // VS AnyCode and Open With Visual Studio
    bool ok1 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\Background\\shellex\\ContextMenuHandlers", L"Open With Visual Studio", enable);
    if (enable) {
        if (RegKeyExists(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell\\AnyCode")) {
            ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell", L"AnyCode", true);
        }
    } else {
        if (RegKeyExists(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell\\-AnyCode")) {
            ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\Background\\shell", L"AnyCode", false);
        }
    }
    return ok1;
}

// ============================================================================
// SUB-PANEL 4: NEW MENU (Изменение пунктов в меню Создать - 11 items)
// ============================================================================
bool ContextMenuManager::ApplyNewDelBmp(bool enable) {
    return ToggleShellNew(L".bmp", enable);
}

bool ContextMenuManager::ApplyNewDelContact(bool enable) {
    return ToggleShellNew(L".contact", enable);
}

bool ContextMenuManager::ApplyNewDelRtf(bool enable) {
    return ToggleShellNew(L".rtf", enable);
}

bool ContextMenuManager::ApplyNewDelTxt(bool enable) {
    return ToggleShellNew(L".txt", enable);
}

bool ContextMenuManager::ApplyNewDelZip(bool enable) {
    ToggleShellNew(L".zip", enable);
    return ToggleShellNew(L"CompressedFolder", enable);
}

bool ContextMenuManager::ApplyNewDelBriefcase(bool enable) {
    return ToggleShellNew(L".Briefcase", enable);
}

bool ContextMenuManager::ApplyNewAddCmd(bool enable) {
    return CreateShellNew(L".cmd", enable);
}

bool ContextMenuManager::ApplyNewAddReg(bool enable) {
    return CreateShellNew(L".reg", enable);
}

bool ContextMenuManager::ApplyNewAddVbs(bool enable) {
    return CreateShellNew(L".vbs", enable);
}

bool ContextMenuManager::ApplyNewAddBat(bool enable) {
    return CreateShellNew(L".bat", enable);
}

bool ContextMenuManager::ApplyNewAddPs1(bool enable) {
    return CreateShellNew(L".ps1", enable);
}

// ============================================================================
// SUB-PANEL 5: JUNK CLEAN (Удаление мусорных пунктов - 13 items)
// ============================================================================
bool ContextMenuManager::ApplyJunkPreviousVersions(bool enable) {
    bool ok1 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"AllFilesystemObjects\\shellex\\ContextMenuHandlers", L"{596AB062-B4D3-4215-AB88-8C197CEEB8E6}", enable);
    ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Directory\\shellex\\ContextMenuHandlers", L"{596AB062-B4D3-4215-AB88-8C197CEEB8E6}", enable);
    ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Drive\\shellex\\ContextMenuHandlers", L"{596AB062-B4D3-4215-AB88-8C197CEEB8E6}", enable);
    return ok1;
}

bool ContextMenuManager::ApplyJunkPaint3d(bool enable) {
    bool ok1 = SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"SystemFileAssociations\\image\\shell\\Edit with Paint 3D", enable);
    SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"SystemFileAssociations\\image\\shell\\3D Print", enable);
    return ok1;
}

bool ContextMenuManager::ApplyJunkPinStart(bool enable) {
    bool ok1 = ToggleShellexHandler(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", L"{470C0EBD-DE5C-4ad9-9232-31BF60BE5046}", enable);
    ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Folder\\shellex\\ContextMenuHandlers", L"PintoStartScreen", enable);
    ToggleShellexHandler(HKEY_CLASSES_ROOT, L"exefile\\shellex\\ContextMenuHandlers", L"PintoStartScreen", enable);
    return ok1;
}

bool ContextMenuManager::ApplyJunkPinQuickaccess(bool enable) {
    bool ok1 = SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Folder\\shell\\pintohome", enable);
    SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Directory\\shell\\pintohome", enable);
    return ok1;
}

bool ContextMenuManager::ApplyJunkFindMusic(bool enable) {
    bool ok1 = SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Directory\\shell\\findmusic", enable);
    SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Folder\\shell\\findmusic", enable);
    SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"AudioCD\\shell\\findmusic", enable);
    return ok1;
}

bool ContextMenuManager::ApplyJunkShare(bool enable) {
    return ToggleShellexHandler(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", L"ModernSharing", enable);
}

bool ContextMenuManager::ApplyJunkCast(bool enable) {
    return ToggleShellexHandler(HKEY_CLASSES_ROOT, L"*\\shellex\\ContextMenuHandlers", L"PlayTo", enable);
}

bool ContextMenuManager::ApplyJunkLibrary(bool enable) {
    return ToggleShellexHandler(HKEY_CLASSES_ROOT, L"Folder\\ShellEx\\ContextMenuHandlers", L"Library Location", enable);
}

bool ContextMenuManager::ApplyJunkMapDrive(bool enable) {
    bool ok1 = SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\MapNetworkDrive", enable);
    SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\DisconnectNetworkDrive", enable);
    return ok1;
}

bool ContextMenuManager::ApplyJunkExtractAll(bool enable) {
    return ToggleShellexHandler(HKEY_CLASSES_ROOT, L"CompressedFolder\\ShellEx\\ContextMenuHandlers", L"{b8cdffb0-7ba1-11d0-b4e6-0000f800e73e}", enable);
}

bool ContextMenuManager::ApplyJunkAddFavorites(bool enable) {
    return SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"*\\shell\\pintohomefile", enable);
}

bool ContextMenuManager::ApplyJunkBitlocker(bool enable) {
    bool ok1 = SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Drive\\shell\\encrypt-bde", enable);
    SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"Drive\\shell\\encrypt-bde-elev", enable);
    return ok1;
}

bool ContextMenuManager::ApplyJunkManage(bool enable) {
    return SetProgrammaticAccessOnly(HKEY_CLASSES_ROOT, L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shell\\Manage", enable);
}

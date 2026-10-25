#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include "third_party/json.hpp"
#include "system_tweaker.h"

// Forwarding alias for compatibility with task specifications
using SystemTweaks = SystemTweaker;

struct ContextMenuTweakItem {
    std::string id;
    std::string title;
    std::string description;
    bool enabled = false;
    bool shiftOnly = false; // "Extended" registry key for Shift+ПКМ
};

class ContextMenuManager {
public:
    static ContextMenuManager& Get();

    void Init();
    void DetectState();

    nlohmann::json GetStateJson() const;
    bool HasTweak(const std::string& id) const;

    bool SetTweak(const std::string& id, bool enabled, std::string& outMsg);
    bool SetShiftOnly(const std::string& id, bool shiftOnly, std::string& outMsg);
    bool ToggleTweak(const std::string& id, bool enabled, std::string& outMsg);
    bool ApplyAll(bool enable, std::string& outMsg);
    bool ApplyGroup(const std::string& group, bool enable, std::string& outMsg);

private:
    ContextMenuManager();
    ~ContextMenuManager() = default;

    mutable std::mutex m_mutex;
    std::vector<ContextMenuTweakItem> m_items;

    // Registry helper methods
    static bool RegKeyExists(HKEY hRoot, const std::wstring& subKey);
    static bool RegValueExists(HKEY hRoot, const std::wstring& subKey, const std::wstring& valName);
    static bool RegDeleteTreeNative(HKEY hRoot, const std::wstring& subKey);
    static bool SetShellVerb(HKEY hRoot, const std::wstring& path, const std::wstring& menuText,
                             const std::wstring& cmd, const std::wstring& icon = L"",
                             bool shiftOnly = false, bool hasShield = false);
    static bool RemoveShellVerb(HKEY hRoot, const std::wstring& path);
    static bool UpdateExtendedFlag(HKEY hRoot, const std::wstring& path, bool shiftOnly);

    // Advanced Registry Helpers for Sub-Panels
    static bool SetProgrammaticAccessOnly(HKEY hRoot, const std::wstring& keyPath, bool hide);
    static bool HasProgrammaticAccessOnly(HKEY hRoot, const std::wstring& keyPath);
    static bool ToggleShellexHandler(HKEY hRoot, const std::wstring& parentPath, const std::wstring& handlerName, bool hide);
    static bool IsShellexHandlerHidden(HKEY hRoot, const std::wstring& parentPath, const std::wstring& handlerName);
    static bool ToggleShellNew(const std::wstring& ext, bool disable);
    static bool IsShellNewDisabled(const std::wstring& ext);
    static bool CreateShellNew(const std::wstring& ext, bool enable);
    static bool HasShellNew(const std::wstring& ext);

    static std::wstring GetCurrentExePath();
    static void NotifyShellUpdate();

    // Specific tweak appliers - Main 16 Tweaks
    bool ApplyEmptyFolder(bool enable, bool shiftOnly);
    bool ApplySymlink(bool enable, bool shiftOnly);
    bool ApplyTakeOwnershipUser(bool enable, bool shiftOnly);
    bool ApplyTakeOwnershipAdmin(bool enable, bool shiftOnly);
    bool ApplyBlockNetAccess(bool enable, bool shiftOnly);
    bool ApplyCmdPsAnywhere(bool enable, bool shiftOnly);
    bool ApplyInstallCab(bool enable, bool shiftOnly);
    bool ApplyHashFile(bool enable, bool shiftOnly);
    bool ApplyCompactSeparate(bool enable, bool shiftOnly);
    bool ApplyCopyImageBuffer(bool enable, bool shiftOnly);
    bool ApplyPermanentDelete(bool enable, bool shiftOnly);
    bool ApplyCleanJunkContext(bool enable, bool shiftOnly);
    bool ApplyTaskMgrContext(bool enable, bool shiftOnly);

    // Sub-Panel 1: This PC (Computer) Tweaks
    bool ApplyCompAdmin(bool enable);
    bool ApplyCompDevmgmt(bool enable);
    bool ApplyCompControl(bool enable);
    bool ApplyCompAppwiz(bool enable);
    bool ApplyCompGpedit(bool enable);
    bool ApplyCompRegedit(bool enable);
    bool ApplyCompServices(bool enable);
    bool ApplyCompDiskmgmt(bool enable);
    bool ApplyCompMsconfig(bool enable);
    bool ApplyCompTaskschd(bool enable);
    bool ApplyCompWinver(bool enable);
    bool ApplyCompEventvwr(bool enable);
    bool ApplyCompSysprops(bool enable);
    bool ApplyCompReplaceProps(bool enable);
    bool ApplyCompHosts(bool enable);
    bool ApplyCompNetconnections(bool enable);
    bool ApplyCompPerfmon(bool enable);
    bool ApplyCompResmon(bool enable);
    bool ApplyCompGodmode(bool enable, bool shiftOnly);

    // Sub-Panel 2: Desktop Add Tweaks
    bool ApplyDeskRestartExplorer(bool enable);
    bool ApplyDeskRefreshIcons(bool enable);
    bool ApplyDeskCompactOs(bool enable, bool shiftOnly);
    bool ApplyDeskPowerSchemes(bool enable, bool shiftOnly);
    bool ApplyDeskSafeMode(bool enable, bool shiftOnly);
    bool ApplyDeskToggleNet(bool enable, bool shiftOnly);
    bool ApplyDeskToggleHidden(bool enable, bool shiftOnly);
    bool ApplyDeskToggleExt(bool enable, bool shiftOnly);
    bool ApplyDeskSafeRemove(bool enable, bool shiftOnly);
    bool ApplyDeskScreenClip(bool enable, bool shiftOnly);
    bool ApplyDeskRunAsTi(bool enable, bool shiftOnly);
    bool ApplyDeskThemeToggle(bool enable);
    bool ApplyDeskDisplaySwitch(bool enable);
    bool ApplyDeskScreenOff(bool enable);
    bool ApplyDeskPowerMenu(bool enable);
    bool ApplyDeskClassicPersonalize(bool enable);

    // Sub-Panel 3: Desktop Del (Hide Driver items)
    bool ApplyDelAmd(bool enable);
    bool ApplyDelIntel(bool enable);
    bool ApplyDelNvidia(bool enable);
    bool ApplyDelVs(bool enable);

    // Sub-Panel 4: New Menu Tweaks
    bool ApplyNewDelBmp(bool enable);
    bool ApplyNewDelContact(bool enable);
    bool ApplyNewDelRtf(bool enable);
    bool ApplyNewDelTxt(bool enable);
    bool ApplyNewDelZip(bool enable);
    bool ApplyNewDelBriefcase(bool enable);
    bool ApplyNewAddCmd(bool enable);
    bool ApplyNewAddReg(bool enable);
    bool ApplyNewAddVbs(bool enable);
    bool ApplyNewAddBat(bool enable);
    bool ApplyNewAddPs1(bool enable);

    // Sub-Panel 5: Junk Clean Tweaks
    bool ApplyJunkPreviousVersions(bool enable);
    bool ApplyJunkPaint3d(bool enable);
    bool ApplyJunkPinStart(bool enable);
    bool ApplyJunkPinQuickaccess(bool enable);
    bool ApplyJunkFindMusic(bool enable);
    bool ApplyJunkShare(bool enable);
    bool ApplyJunkCast(bool enable);
    bool ApplyJunkLibrary(bool enable);
    bool ApplyJunkMapDrive(bool enable);
    bool ApplyJunkExtractAll(bool enable);
    bool ApplyJunkAddFavorites(bool enable);
    bool ApplyJunkBitlocker(bool enable);
    bool ApplyJunkManage(bool enable);
};

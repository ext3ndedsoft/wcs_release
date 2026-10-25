#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include "common_utils.h"

#include "resource.h"
#include "tweaks_engine.h"
#include "system_info.h"
#include "system_tweaker.h"
#include "config_manager.h"
#include "memory_cleaner.h"
#include "autostart_manager.h"
#include "debug_logger.h"
#include "debug_console.h"
#include "native_bridge.h"
#include "timer_resolution.h"
#include "game_boost_manager.h"
#include "update_manager.h"
#include "webview_window.h"

#include <fstream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <thread>

#define WM_TRAY_ICON         (WM_USER + 101)
#define TIMER_TELEMETRY_ID   201
#define TIMER_IDLE_TRIM_ID   202

#define ID_TRAY_OPEN_MAIN         2001
#define ID_TRAY_OPEN_MEM_CLEANER  2002
#define ID_TRAY_TRIM_WS           2003
#define ID_TRAY_CLEAR_CACHE       2004
#define ID_TRAY_CLEAN_ALL         2005
#define ID_TRAY_EXIT              2006

HWND g_hwndMain = nullptr;
HWND g_hwndMemCleaner = nullptr;

static WebViewWindow* g_pMainWebView = nullptr;
static WebViewWindow* g_pMemCleanerWebView = nullptr;

static HICON g_hIconSmall = nullptr;
static NOTIFYICONDATAW g_nid = { sizeof(g_nid) };
static bool g_trayIconActive = false;

void RemoveTrayIcon() {
    if (g_trayIconActive) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconActive = false;
    }
}

void AddTrayIcon(HWND hwnd) {
    if (!g_trayIconActive && hwnd) {
        g_nid.cbSize = sizeof(NOTIFYICONDATAW);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAY_ICON;
        g_nid.hIcon = g_hIconSmall;
        wcscpy_s(g_nid.szTip, L"Windows Config Studio");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_trayIconActive = true;
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        std::filesystem::path logDir = std::filesystem::path(localAppData) / L"WindowsConfigStudio" / L"logs";
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);
        std::ofstream f(logDir / "crash_log.txt", std::ios::app);
        if (f.is_open()) {
            f << "CRASH! Code: 0x" << std::hex << ep->ExceptionRecord->ExceptionCode 
              << " at address: 0x" << ep->ExceptionRecord->ExceptionAddress << "\n";
            f.close();
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    // Attach thread to interactive user desktop
    HDESK hDefault = OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
    if (hDefault) {
        SetThreadDesktop(hDefault);
    }
    OleInitialize(NULL);
    DisableProcessWindowsGhosting();
    SetUnhandledExceptionFilter(CrashFilter);
#ifdef WCS_DEBUG_BUILD
    DebugLogger::Get().Init(true);
#else
    DebugLogger::Get().Init(false);
#endif

    // Handle CLI utility arguments (e.g. triggered from Explorer Context Menu)
    if (pCmdLine && (wcsstr(pCmdLine, L"--mem-clean") != nullptr || wcsstr(pCmdLine, L"-mem-clean") != nullptr)) {
        size_t freed = 0;
        std::string status;
        MemoryCleaner::Get().CleanAll(freed, status);
        std::string sFreed = MemoryCleaner::FormatBytes(freed);
        std::wstring msg = L"Очистка оперативной памяти успешно завершена.\nОсвобождено: " +
            std::wstring(sFreed.begin(), sFreed.end());
        MessageBoxW(nullptr, msg.c_str(), L"Windows Config Studio - Очистка ОЗУ", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (pCmdLine && (wcsstr(pCmdLine, L"--clean-junk") != nullptr || wcsstr(pCmdLine, L"-clean-junk") != nullptr)) {
        std::string msg1, msg2;
        SystemTweaker::Get().CleanJunkFolders(msg1);
        SystemTweaker::Get().CleanRegistryJunk(msg2);
        std::wstring msg = L"Экспресс-очистка временных файлов и мусора завершена.\n" + Utf8ToWideStr(msg1);
        MessageBoxW(nullptr, msg.c_str(), L"Windows Config Studio - Очистка мусора", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (pCmdLine && (wcsstr(pCmdLine, L"--benchmark-cleanup") != nullptr)) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
        CleanupOptions opts;
        opts.cleanDefenderCache = true;
        opts.cleanUsbHistory = true;
        opts.cleanWindowsInstallCache = true;
        opts.cleanEventLogs = true;
        opts.cleanSoftwareDistribution = true;
        opts.cleanTempFiles = true;
        opts.cleanExplorerThumbnails = true;
        opts.cleanDiagnosticLogs = true;
        opts.cleanBrowserCache = true;
        opts.cleanSystemReports = true;
        opts.cleanOfficeCache = true;
        opts.cleanGpuCache = true;

        auto t0 = std::chrono::high_resolution_clock::now();
        uint64_t freedBytes = 0;
        std::string msg;
        bool ok = SystemTweaker::Get().ExecuteCleanup(opts, freedBytes, msg);
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::ofstream logFile("cleanup_benchmark.log");
        if (logFile.is_open()) {
            logFile << "[BENCHMARK] Success: " << (ok ? "true" : "false") << "\n";
            logFile << "[BENCHMARK] Elapsed: " << ms << " ms (" << (ms / 1000.0) << " s)\n";
            logFile << "[BENCHMARK] Freed bytes: " << freedBytes << "\n";
            logFile << "[BENCHMARK] Message: " << msg << "\n";
            logFile.close();
        }

        std::cout << "\n[BENCHMARK] Success: " << (ok ? "true" : "false") << "\n";
        std::cout << "[BENCHMARK] Elapsed: " << ms << " ms (" << (ms / 1000.0) << " s)\n";
        std::cout << "[BENCHMARK] Freed bytes: " << freedBytes << "\n";
        std::cout << "[BENCHMARK] Message: " << msg << "\n" << std::flush;
        return 0;
    }

    // Single-instance protection using a named mutex
    HANDLE hSingleInstanceMutex = CreateMutexW(NULL, TRUE, L"Local\\WindowsConfigStudio_WebView_SingleInstance_Mutex_v2");
    if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_ACCESS_DENIED) {
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);

        bool isAutostart = (pCmdLine && (wcsstr(pCmdLine, L"--autostart") != nullptr || wcsstr(pCmdLine, L"-autostart") != nullptr));
        if (isAutostart) {
            return 0; // Silently exit duplicate autostart instance
        }

        // Restore and focus the existing window
        HWND existingHwnd = FindWindowW(L"WindowsConfigStudioWebViewClass", nullptr);
        if (existingHwnd) {
            if (IsIconic(existingHwnd)) {
                ShowWindow(existingHwnd, SW_RESTORE);
            } else {
                ShowWindow(existingHwnd, SW_SHOWNORMAL);
            }
            SetForegroundWindow(existingHwnd);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HICON hIconBig = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!hIconBig) hIconBig = (HICON)LoadImageW(nullptr, L"app_icon.ico", IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
    if (!hIconSmall) hIconSmall = (HICON)LoadImageW(nullptr, L"app_icon.ico", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    if (!hIconBig) hIconBig = (HICON)LoadImageW(nullptr, L"resources\\branding\\app_icon.ico", IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
    if (!hIconSmall) hIconSmall = (HICON)LoadImageW(nullptr, L"resources\\branding\\app_icon.ico", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    g_hIconSmall = hIconSmall;

    HBRUSH hDarkBrush = CreateSolidBrush(RGB(10, 12, 16));

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        hInstance,
        hIconBig,
        LoadCursor(nullptr, IDC_ARROW),
        hDarkBrush,
        nullptr,
        L"WindowsConfigStudioWebViewClass",
        hIconSmall
    };
    RegisterClassExW(&wc);

    const int windowWidth = 1300;
    const int windowHeight = 920;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenWidth - windowWidth) / 2;
    int posY = (screenHeight - windowHeight) / 2;

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Windows Config Studio",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        posX,
        posY,
        windowWidth,
        windowHeight,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );
    g_hwndMain = hwnd;
    NativeBridge::Get().SetMainHwnd(hwnd);

    if (hIconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));
    COLORREF darkBorder = RGB(22, 27, 38);
    DwmSetWindowAttribute(hwnd, 34, &darkBorder, sizeof(darkBorder));

    // Dedicated Memory Cleaner Window (420x590)
    const int memWidth = 420;
    const int memHeight = 590;
    RECT memRc = { 0, 0, memWidth, memHeight };
    AdjustWindowRectEx(&memRc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    int memActualW = memRc.right - memRc.left;
    int memActualH = memRc.bottom - memRc.top;
    int memX = (screenWidth - memActualW) / 2 + 60;
    int memY = (screenHeight - memActualH) / 2 + 60;

    g_hwndMemCleaner = CreateWindowW(
        wc.lpszClassName,
        L"Memory Cleaner",
        (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) | WS_CLIPCHILDREN,
        memX,
        memY,
        memActualW,
        memActualH,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    if (hIconBig) SendMessageW(g_hwndMemCleaner, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(g_hwndMemCleaner, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    DwmSetWindowAttribute(g_hwndMemCleaner, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    DwmSetWindowAttribute(g_hwndMemCleaner, 33, &cornerPref, sizeof(cornerPref));
    DwmSetWindowAttribute(g_hwndMemCleaner, 34, &darkBorder, sizeof(darkBorder));

    // Initialize Config & Engine fast states
    ConfigManager::Get().Load();
    TweaksEngine::Get().DetectFastState();
    SystemTweaker::Get().DetectFastState();

    if (ConfigManager::Get().GetConfig().tweaks.timerResolution05ms) {
        TimerResolutionService::Get().Start(0.500f);
    }
    GameBoostManager::Get().Start();
    UpdateManager::Get().Init();

    // Setup NativeBridge callback for opening Memory Cleaner window (lazy-loaded on demand)
    NativeBridge::Get().SetOpenMemCleanerCallback([]() {
        if (g_hwndMemCleaner) {
            if (!g_pMemCleanerWebView) {
                g_pMemCleanerWebView = new WebViewWindow(g_hwndMemCleaner, L"https://wcs.local/web/mem_cleaner.html", false);
                g_pMemCleanerWebView->Init([](bool success) {
                    if (success && g_hwndMemCleaner) {
                        ShowWindow(g_hwndMemCleaner, SW_SHOWNORMAL);
                        UpdateWindow(g_hwndMemCleaner);
                        SetForegroundWindow(g_hwndMemCleaner);
                    }
                });
            } else {
                g_pMemCleanerWebView->Resume();
                if (IsIconic(g_hwndMemCleaner)) {
                    ShowWindow(g_hwndMemCleaner, SW_RESTORE);
                } else {
                    ShowWindow(g_hwndMemCleaner, SW_SHOWNORMAL);
                }
                UpdateWindow(g_hwndMemCleaner);
                SetForegroundWindow(g_hwndMemCleaner);
                NativeBridge::Get().BroadcastTelemetry();
            }
        }
    });

    bool isAutostart = (pCmdLine && (wcsstr(pCmdLine, L"--autostart") != nullptr || wcsstr(pCmdLine, L"-autostart") != nullptr));

    bool shouldStartHidden = isAutostart || ConfigManager::Get().GetConfig().startMinimizedToTray;
    bool openMemDirectly = (pCmdLine && (wcsstr(pCmdLine, L"--memcleaner") != nullptr || wcsstr(pCmdLine, L"--memory") != nullptr));
    if (openMemDirectly) {
        shouldStartHidden = true;
    }
    if (!shouldStartHidden) {
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }

    // Create Main WebView
    g_pMainWebView = new WebViewWindow(hwnd, L"https://wcs.local/web/index.html", true);
    g_pMainWebView->Init([shouldStartHidden](bool success) {
        if (!success) {
            DEBUG_LOG_ERROR("MAIN", "Main WebView2 initialization failed");
        } else if (shouldStartHidden) {
            if (g_pMainWebView) g_pMainWebView->Suspend();
        }
    });

    AddTrayIcon(hwnd);

    if (openMemDirectly) {
        NativeBridge::Get().OpenMemCleanerWindow();
    }

    // Launch asynchronous background discovery thread
    std::thread bgDiscoveryThread([]() {
        AutostartManager::Get().SyncStartupState();
        TweaksEngine::Get().DetectCurrentStateAsync();
        SystemInfo::Get().RefreshFullAsync([]() {
            NativeBridge::Get().BroadcastTelemetry();
        });
        SystemTweaker::Get().InitAsync();
    });
    bgDiscoveryThread.detach();

    // High-resolution telemetry & memory timer (every 1000ms)
    SetTimer(hwnd, TIMER_TELEMETRY_ID, 1000, nullptr);

    // Event Loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(hwnd, TIMER_TELEMETRY_ID);

    GameBoostManager::Get().Stop();
    TimerResolutionService::Get().Stop();

    if (g_pMemCleanerWebView) {
        delete g_pMemCleanerWebView;
        g_pMemCleanerWebView = nullptr;
    }
    if (g_pMainWebView) {
        delete g_pMainWebView;
        g_pMainWebView = nullptr;
    }

    if (g_hwndMemCleaner) DestroyWindow(g_hwndMemCleaner);
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    DeleteObject(hDarkBrush);

    if (hSingleInstanceMutex) {
        ReleaseMutex(hSingleInstanceMutex);
        CloseHandle(hSingleInstanceMutex);
    }

    OleUninitialize();
    return (int)msg.wParam;
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case (WM_USER + 150):
        NativeBridge::Get().ProcessUITasks();
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            // Window lost focus: schedule memory trim after 3 seconds of inactivity
            SetTimer(hWnd, TIMER_IDLE_TRIM_ID, 3000, nullptr);
        } else {
            // Window regained focus: cancel idle trim, resume if suspended
            KillTimer(hWnd, TIMER_IDLE_TRIM_ID);
            if (hWnd == g_hwndMain && g_pMainWebView && g_pMainWebView->IsSuspended()) {
                g_pMainWebView->Resume();
            } else if (hWnd == g_hwndMemCleaner && g_pMemCleanerWebView && g_pMemCleanerWebView->IsSuspended()) {
                g_pMemCleanerWebView->Resume();
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_IDLE_TRIM_ID) {
            KillTimer(hWnd, TIMER_IDLE_TRIM_ID);
            WebViewWindow::TrimProcessMemory();
            return 0;
        }
        if (wParam == TIMER_TELEMETRY_ID) {
            MemoryCleaner::Get().Update(1.0f);

            bool isMainVis = (g_hwndMain && IsWindowVisible(g_hwndMain) && !IsIconic(g_hwndMain));
            bool isMemVis = (g_hwndMemCleaner && IsWindowVisible(g_hwndMemCleaner) && !IsIconic(g_hwndMemCleaner));
            if (isMainVis || isMemVis) {
                HWND fg = GetForegroundWindow();
                bool isFocused = (fg == g_hwndMain || fg == g_hwndMemCleaner);
                static int s_skipCounter = 0;
                // If focused, send every 1000ms. If unfocused in background, throttle to every 3000ms (saves CPU)
                if (isFocused || (++s_skipCounter % 3 == 0)) {
                    NativeBridge::Get().BroadcastTelemetry();
                }
            }
        }
        return 0;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (ConfigManager::Get().GetConfig().runInBackground) {
                ShowWindow(hWnd, SW_HIDE);
            }
            if (hWnd == g_hwndMain && g_pMainWebView) {
                g_pMainWebView->Suspend();
            } else if (hWnd == g_hwndMemCleaner && g_pMemCleanerWebView) {
                g_pMemCleanerWebView->Suspend();
            }
            WebViewWindow::TrimProcessMemory();
            return 0;
        } else {
            if (hWnd == g_hwndMain && g_pMainWebView) {
                if (g_pMainWebView->IsSuspended()) g_pMainWebView->Resume();
                g_pMainWebView->Resize();
            } else if (hWnd == g_hwndMemCleaner && g_pMemCleanerWebView) {
                if (g_pMemCleanerWebView->IsSuspended()) g_pMemCleanerWebView->Resume();
                g_pMemCleanerWebView->Resize();
            }
        }
        return 0;

    case WM_SETFOCUS:
        if (hWnd == g_hwndMain && g_pMainWebView) {
            if (g_pMainWebView->IsSuspended()) g_pMainWebView->Resume();
            g_pMainWebView->Focus();
        } else if (hWnd == g_hwndMemCleaner && g_pMemCleanerWebView) {
            if (g_pMemCleanerWebView->IsSuspended()) g_pMemCleanerWebView->Resume();
            g_pMemCleanerWebView->Focus();
        }
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        static HBRUSH s_darkBrush = CreateSolidBrush(RGB(10, 12, 16));
        FillRect(hdc, &rc, s_darkBrush);
        return 1;
    }

    case WM_CLOSE:
        if (hWnd == g_hwndMemCleaner) {
            ShowWindow(hWnd, SW_HIDE);
            if (g_pMemCleanerWebView) {
                g_pMemCleanerWebView->Suspend();
            }
            return 0;
        } else {
            if (ConfigManager::Get().GetConfig().runInBackground) {
                ShowWindow(hWnd, SW_HIDE);
                if (g_pMainWebView) {
                    g_pMainWebView->Suspend();
                }
                return 0;
            }
            DestroyWindow(hWnd);
            return 0;
        }

    case WM_TRAY_ICON:
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            if (g_hwndMain) {
                if (g_pMainWebView) g_pMainWebView->Resume();
                if (IsIconic(g_hwndMain)) ShowWindow(g_hwndMain, SW_RESTORE);
                else ShowWindow(g_hwndMain, SW_SHOWNORMAL);
                SetForegroundWindow(g_hwndMain);
                NativeBridge::Get().BroadcastTelemetry();
            }
            return 0;
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_OPEN_MAIN, L"🚀 Открыть Windows Config Studio");
            InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, ID_TRAY_OPEN_MEM_CLEANER, L"⚡ Memory Cleaner (Очистка ОЗУ)");
            InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_STRING, ID_TRAY_TRIM_WS, L"🧹 Trim Processes' Working Set");
            InsertMenuW(hMenu, 4, MF_BYPOSITION | MF_STRING, ID_TRAY_CLEAR_CACHE, L"💾 Clear System Cache");
            InsertMenuW(hMenu, 5, MF_BYPOSITION | MF_STRING, ID_TRAY_CLEAN_ALL, L"✨ Полная оптимизация памяти");
            InsertMenuW(hMenu, 6, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"❌ Выход");

            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);

            if (cmd == ID_TRAY_OPEN_MAIN) {
                if (g_hwndMain) {
                    if (g_pMainWebView) g_pMainWebView->Resume();
                    if (IsIconic(g_hwndMain)) ShowWindow(g_hwndMain, SW_RESTORE);
                    else ShowWindow(g_hwndMain, SW_SHOWNORMAL);
                    SetForegroundWindow(g_hwndMain);
                    NativeBridge::Get().BroadcastTelemetry();
                }
            } else if (cmd == ID_TRAY_OPEN_MEM_CLEANER) {
                NativeBridge::Get().OpenMemCleanerWindow();
            } else if (cmd == ID_TRAY_TRIM_WS) {
                size_t freed = 0;
                std::string s;
                MemoryCleaner::Get().TrimProcessesWorkingSet(freed, s);
                WebViewWindow::TrimProcessMemory();
                NativeBridge::Get().ShowToast(s, false);
                NativeBridge::Get().BroadcastTelemetry();
            } else if (cmd == ID_TRAY_CLEAR_CACHE) {
                size_t freed = 0;
                std::string s;
                MemoryCleaner::Get().ClearSystemCache(freed, s);
                WebViewWindow::TrimProcessMemory();
                NativeBridge::Get().ShowToast(s, false);
                NativeBridge::Get().BroadcastTelemetry();
            } else if (cmd == ID_TRAY_CLEAN_ALL) {
                size_t freed = 0;
                std::string s;
                MemoryCleaner::Get().CleanAll(freed, s);
                WebViewWindow::TrimProcessMemory();
                NativeBridge::Get().ShowToast(s, false);
                NativeBridge::Get().BroadcastTelemetry();
            } else if (cmd == ID_TRAY_EXIT) {
                if (g_hwndMemCleaner) DestroyWindow(g_hwndMemCleaner);
                if (g_hwndMain) DestroyWindow(g_hwndMain);
            }
        }
        return 0;

    case WM_DESTROY:
        if (hWnd == g_hwndMain) {
            RemoveTrayIcon();
            PostQuitMessage(0);
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

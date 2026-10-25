#include "webview_window.h"
#include "common_utils.h"
#include "resource_bundle.h"
#include "native_bridge.h"
#include "debug_logger.h"

#include <shlobj.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <vector>

#include "third_party/webview2/build/native/include/WebView2EnvironmentOptions.h"

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Environment> s_sharedEnv = nullptr;
static bool s_envCreating = false;
static std::vector<std::function<void(ICoreWebView2Environment*)>> s_envWaiters;

#ifdef WCS_DEBUG_BUILD
static void LogWV(const std::string& msg) {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        std::filesystem::path logDir = std::filesystem::path(localAppData) / L"WindowsConfigStudio" / L"logs";
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);
        std::ofstream f(logDir / "webview_debug.log", std::ios::app);
        if (f.is_open()) {
            f << msg << "\n";
            f.flush();
        }
    }
    DEBUG_LOG_INFO("WebView", msg);
}
#else
#define LogWV(msg) ((void)0)
#endif

WebViewWindow::WebViewWindow(HWND hWnd, const std::wstring& initialUrl, bool isMain)
    : m_hWnd(hWnd), m_initialUrl(initialUrl), m_isMain(isMain) {
}

WebViewWindow::~WebViewWindow() {
    if (m_webView) {
        NativeBridge::Get().UnregisterWebView(m_webView.Get());
    }
    if (m_controller) {
        m_controller->Close();
    }
}

std::wstring WebViewWindow::GetWebRootPath() const {
    return ResourceBundle::GetResourcesDirectory();
}

std::wstring WebViewWindow::GetUserDataDir() const {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        std::filesystem::path p(localAppData);
        p /= L"WindowsConfigStudio";
        p /= L"WebView2Profile";
        std::filesystem::create_directories(p);
        return p.wstring();
    }
    return L"";
}

bool WebViewWindow::Init(std::function<void(bool success)> onInitCompleted) {
    LogWV("Init called for window (isMain=" + std::to_string(m_isMain) + ", HWND=" + std::to_string((uintptr_t)m_hWnd) + ")");

    // Ensure thread is attached to the interactive Default desktop
    HDESK hDefault = OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
    if (hDefault) {
        SetThreadDesktop(hDefault);
    }

    if (m_hWnd && IsWindow(m_hWnd)) {
        LONG_PTR style = GetWindowLongPtrW(m_hWnd, GWL_STYLE);
        if (!(style & WS_CLIPCHILDREN)) {
            SetWindowLongPtrW(m_hWnd, GWL_STYLE, style | WS_CLIPCHILDREN);
        }
    }

    auto createControllerWithEnv = [this, onInitCompleted](ICoreWebView2Environment* env) {
        if (!env) {
            LogWV("ERROR: env is null!");
            if (onInitCompleted) onInitCompleted(false);
            return;
        }

        LogWV("Calling CreateCoreWebView2Controller on HWND " + std::to_string((uintptr_t)m_hWnd));
        env->CreateCoreWebView2Controller(
            m_hWnd,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, onInitCompleted](HRESULT cResult, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(cResult) || !controller) {
                        LogWV("ERROR: CreateCoreWebView2Controller failed: 0x" + std::to_string(cResult));
                        if (onInitCompleted) onInitCompleted(false);
                        return cResult;
                    }

                    LogWV("CreateCoreWebView2Controller SUCCEEDED!");
                    m_controller = controller;
                    m_controller->get_CoreWebView2(&m_webView);

                    // Dark background to eliminate white flashes
                    ComPtr<ICoreWebView2Controller2> controller2;
                    if (SUCCEEDED(m_controller.As(&controller2)) && controller2) {
                        COREWEBVIEW2_COLOR darkCol = { 255, 10, 12, 16 };
                        controller2->put_DefaultBackgroundColor(darkCol);
                    }

                    // Virtual Host mapping for wcs.local -> resources folder
                    std::wstring webRoot = GetWebRootPath();
                    LogWV("WebRoot resolved: " + WideStrToUtf8(webRoot));

                    ComPtr<ICoreWebView2_3> webView3;
                    if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
                        HRESULT mapHr = webView3->SetVirtualHostNameToFolderMapping(
                            L"wcs.local",
                            webRoot.c_str(),
                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
                        );
                        LogWV("SetVirtualHostNameToFolderMapping result: 0x" + std::to_string(mapHr));
                    } else {
                        LogWV("WARNING: ICoreWebView2_3 not supported!");
                    }

                    // Configure Settings
                    ComPtr<ICoreWebView2Settings> settings;
                    if (SUCCEEDED(m_webView->get_Settings(&settings)) && settings) {
#ifdef WCS_DEBUG_BUILD
                        settings->put_AreDefaultContextMenusEnabled(TRUE);
                        settings->put_AreDevToolsEnabled(TRUE);
#else
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(FALSE);
#endif
                        settings->put_IsZoomControlEnabled(FALSE);
                        settings->put_IsBuiltInErrorPageEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE);
                    }

                    // Navigation Completed Listener
                    m_webView->add_NavigationCompleted(
                        Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                BOOL isSuccess = FALSE;
                                args->get_IsSuccess(&isSuccess);
                                COREWEBVIEW2_WEB_ERROR_STATUS status;
                                args->get_WebErrorStatus(&status);
                                LogWV("NavigationCompleted! success=" + std::to_string(isSuccess) + ", status=" + std::to_string(status));

                                // Set memory target level to LOW immediately upon navigation
                                ComPtr<ICoreWebView2_19> webView19;
                                if (m_webView && SUCCEEDED(m_webView.As(&webView19)) && webView19) {
                                    webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
                                }

                                // Delay memory trim by 2.0s so initial rendering and DOM setup settle, then purge startup working set
                                std::thread([this]() {
                                    Sleep(2000);
                                    ComPtr<ICoreWebView2_19> w19;
                                    if (m_webView && SUCCEEDED(m_webView.As(&w19)) && w19) {
                                        w19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
                                    }
                                    TrimProcessMemory();
                                }).detach();

                                return S_OK;
                            }).Get(), nullptr
                    );

                    // Suppress any default browser script dialogs (alert, confirm, prompt)
                    m_webView->add_ScriptDialogOpening(
                        Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
                            [](ICoreWebView2* sender, ICoreWebView2ScriptDialogOpeningEventArgs* args) -> HRESULT {
                                if (args) {
                                    args->Accept();
                                }
                                return S_OK;
                            }).Get(), nullptr
                    );

                    // Hook WebMessage handler
                    EventRegistrationToken token;
                    m_webView->add_WebMessageReceived(
                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                LPWSTR msg = nullptr;
                                args->get_WebMessageAsJson(&msg);
                                if (msg) {
                                    NativeBridge::Get().HandleWebMessage(sender, msg);
                                    CoTaskMemFree(msg);
                                }
                                return S_OK;
                            }).Get(),
                        &token
                    );

                    NativeBridge::Get().RegisterWebView(m_webView.Get(), m_isMain);

                    Resize();
                    m_controller->put_IsVisible(TRUE);

                    // Ensure PrintScreen (VK_SNAPSHOT) is never swallowed by WebView2
                    EventRegistrationToken accelToken;
                    m_controller->add_AcceleratorKeyPressed(
                        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                            [](ICoreWebView2Controller* sender, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                                UINT key = 0;
                                if (SUCCEEDED(args->get_VirtualKey(&key)) && key == VK_SNAPSHOT) {
                                    args->put_Handled(FALSE);
                                }
                                return S_OK;
                            }).Get(),
                        &accelToken
                    );

                    if (!m_initialUrl.empty()) {
                        LogWV("Navigating to: " + WideStrToUtf8(m_initialUrl));
                        m_webView->Navigate(m_initialUrl.c_str());
                    }

                    m_isReady = true;
                    if (onInitCompleted) onInitCompleted(true);
                    return S_OK;
                }).Get()
        );
    };

    if (s_sharedEnv) {
        createControllerWithEnv(s_sharedEnv.Get());
        return true;
    }

    s_envWaiters.push_back(createControllerWithEnv);

    if (s_envCreating) {
        return true;
    }

    s_envCreating = true;
    std::wstring userDataDir = GetUserDataDir();
    LogWV("Creating shared CoreWebView2Environment at: " + WideStrToUtf8(userDataDir));

    auto options = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--disable-features=RendererCodeIntegrity,PreloadMediaEngagementData,Translate,MediaRouter,OptimizationHints,InterestFeedContentSuggestions,CalculateNativeWinOcclusion,AutofillServerCommunication,SpareRendererForSitePerProcess,CertificateTransparencyComponentUpdater "
        L"--autoplay-policy=no-user-gesture-required "
        L"--renderer-process-limit=1 "
        L"--disable-background-networking "
        L"--disable-component-update "
        L"--disable-domain-reliability "
        L"--disable-sync "
        L"--disable-speech-api "
        L"--disable-breakpad "
        L"--disable-logging "
        L"--disable-extensions "
        L"--disable-default-apps "
        L"--no-default-browser-check "
        L"--disable-hang-monitor "
        L"--disable-client-side-phishing-detection "
        L"--disable-component-extensions-with-background-pages "
        L"--disable-ipc-flooding-protection "
        L"--enable-features=msWebView2LowMemoryMode,V8MemoryReducer "
        L"--enable-low-end-device-mode "
        L"--js-flags=\"--expose-gc --max-old-space-size=64\" "
        L"--process-per-site"
    );

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataDir.empty() ? nullptr : userDataDir.c_str(),
        options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    LogWV("ERROR: CreateCoreWebView2EnvironmentWithOptions failed: 0x" + std::to_string(result));
                    for (auto& cb : s_envWaiters) cb(nullptr);
                    s_envWaiters.clear();
                    s_envCreating = false;
                    return result;
                }

                LogWV("CreateCoreWebView2EnvironmentWithOptions SUCCEEDED!");
                s_sharedEnv = env;
                s_envCreating = false;

                for (auto& cb : s_envWaiters) {
                    cb(s_sharedEnv.Get());
                }
                s_envWaiters.clear();
                return S_OK;
            }).Get()
    );

    return SUCCEEDED(hr);
}

void WebViewWindow::Resize() {
    if (m_controller && m_hWnd) {
        RECT bounds;
        GetClientRect(m_hWnd, &bounds);
        int w = bounds.right - bounds.left;
        int h = bounds.bottom - bounds.top;
        if (w > 0 && h > 0) {
            m_controller->put_Bounds(bounds);
        }
    }
}

void WebViewWindow::Focus() {
    if (m_controller) {
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void WebViewWindow::Show(bool show) {
    if (m_controller) {
        m_controller->put_IsVisible(show ? TRUE : FALSE);
    }
}

void WebViewWindow::Suspend() {
    if (!m_isReady || !m_controller) return;
    if (m_isSuspended) return;

    m_isSuspended = true;

    // 1. Immediately hide controller to stop Chromium compositor & rendering loops
    m_controller->put_IsVisible(FALSE);

    // 2. Put CoreWebView2 in low memory target level
    ComPtr<ICoreWebView2_19> webView19;
    if (m_webView && SUCCEEDED(m_webView.As(&webView19)) && webView19) {
        webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }

    // 3. Put CoreWebView2 in Suspend state via ICoreWebView2_3
    ComPtr<ICoreWebView2_3> webView3;
    if (m_webView && SUCCEEDED(m_webView.As(&webView3)) && webView3) {
        webView3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>(
            [](HRESULT errorCode, BOOL isSuccessful) -> HRESULT {
                TrimProcessMemory();
                return S_OK;
            }).Get());
    } else {
        TrimProcessMemory();
    }
}

void WebViewWindow::Resume() {
    if (!m_isReady || !m_controller) return;
    if (!m_isSuspended) return;

    m_isSuspended = false;

    // 1. Resume CoreWebView2
    ComPtr<ICoreWebView2_3> webView3;
    if (m_webView && SUCCEEDED(m_webView.As(&webView3)) && webView3) {
        BOOL isSuspended = FALSE;
        if (SUCCEEDED(webView3->get_IsSuspended(&isSuspended)) && isSuspended) {
            webView3->Resume();
        }
    }

    // Keep memory usage target level LOW even while active
    ComPtr<ICoreWebView2_19> webView19;
    if (m_webView && SUCCEEDED(m_webView.As(&webView19)) && webView19) {
        webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }

    // 2. Restore visibility and layout
    m_controller->put_IsVisible(TRUE);
    Resize();
    Focus();
}

void WebViewWindow::TrimProcessMemory() {
    // 1. Purge working set of host process (drops host exe to < 4 MB)
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    // 2. Enumerate and purge all child and grandchild msedgewebview2 processes (Browser, GPU, Renderer, Utility)
    DWORD myPid = GetCurrentProcessId();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        std::vector<PROCESSENTRY32W> procs;
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                procs.push_back(pe);
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);

        // Discover recursive descendant process tree rooted at myPid
        std::vector<DWORD> pidsToTrim;
        std::vector<DWORD> parentPids = { myPid };

        while (!parentPids.empty()) {
            std::vector<DWORD> nextParents;
            for (DWORD parentId : parentPids) {
                for (const auto& p : procs) {
                    if (p.th32ParentProcessID == parentId && p.th32ProcessID != parentId) {
                        pidsToTrim.push_back(p.th32ProcessID);
                        nextParents.push_back(p.th32ProcessID);
                    }
                }
            }
            parentPids = std::move(nextParents);
        }

        // Trim working set across entire process tree
        for (DWORD pid : pidsToTrim) {
            HANDLE hChild = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hChild) {
                SetProcessWorkingSetSize(hChild, (SIZE_T)-1, (SIZE_T)-1);
                CloseHandle(hChild);
            }
        }
    }
}

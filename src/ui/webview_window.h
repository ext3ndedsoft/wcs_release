#pragma once

#include <windows.h>
#include <wrl.h>
#include <string>
#include <functional>
#include "third_party/webview2/build/native/include/WebView2.h"

class WebViewWindow {
public:
    WebViewWindow(HWND hWnd, const std::wstring& initialUrl, bool isMain = true);
    ~WebViewWindow();

    bool Init(std::function<void(bool success)> onInitCompleted = nullptr);
    void Resize();
    void Focus();
    void Show(bool show);
    void Suspend();
    void Resume();
    bool IsSuspended() const { return m_isSuspended; }

    static void TrimProcessMemory();

    HWND GetHwnd() const { return m_hWnd; }
    ICoreWebView2* GetWebView() const { return m_webView.Get(); }
    ICoreWebView2Controller* GetController() const { return m_controller.Get(); }
    bool IsReady() const { return m_isReady; }

private:
    HWND m_hWnd = nullptr;
    std::wstring m_initialUrl;
    bool m_isMain = true;
    bool m_isReady = false;
    bool m_isSuspended = false;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;

    std::wstring GetWebRootPath() const;
    std::wstring GetUserDataDir() const;
};

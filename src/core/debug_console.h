#pragma once

#include <windows.h>

// Entry point for the dedicated standalone debug diagnostic console application.
// When compiled with WCS_DEBUG_BUILD=1, WindowsConfigStudio_Debug.exe launches
// ONLY this interactive console engine (no GUI/DirectX window).
int RunDebugConsoleEngine();

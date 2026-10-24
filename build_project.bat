@echo off
setlocal enabledelayedexpansion

where cl.exe >nul 2>&1
if not errorlevel 1 goto env_ok

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist %VSWHERE% (
    for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_DIR=%%i"
    )
)

if defined VS_DIR (
    if exist "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat" (
        call "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"
        goto env_ok
    )
)

if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    goto env_ok
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    goto env_ok
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    goto env_ok
)
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    goto env_ok
)

echo [ERROR] Failed to load MSVC environment
exit /b 1

:env_ok

echo [1/4] Packing embedded resource bundle...
powershell -ExecutionPolicy Bypass -File "scripts\pack_resources.ps1"
if errorlevel 1 (
    echo [ERROR] Resource packing failed
    exit /b 1
)

powershell -Command "(Get-Item 'src\app.rc').LastWriteTime = (Get-Date)" >nul 2>&1

where cmake.exe >nul 2>&1
if not errorlevel 1 (
    set CMAKE=cmake.exe
) else if defined VS_DIR and exist "!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set CMAKE="!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) else if exist "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) else (
    set CMAKE=cmake.exe
)

where ninja.exe >nul 2>&1
if not errorlevel 1 (
    set NINJA=ninja.exe
) else if defined VS_DIR and exist "!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set NINJA="!VS_DIR!\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
) else if exist "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set NINJA="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
    set NINJA="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
) else (
    set NINJA=ninja.exe
)

echo [2/4] Configuring CMake...
%CMAKE% -B build -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [ERROR] CMake configuration failed
    exit /b 1
)

echo [3/4] Compiling with Ninja...
%NINJA% -C build
if errorlevel 1 (
    echo [ERROR] Ninja build failed
    exit /b 1
)

rem Ensure any running instance is stopped before updating bin
powershell -Command "Stop-Process -Name 'WindowsConfigStudio*', 'WCS*' -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 300" >nul 2>&1

echo [4/4] Deploying standalone WCS.exe binary...
if not exist "bin" mkdir "bin"
powershell -Command "$src = 'build\bin\WCS.exe'; $t = 'bin\WCS.exe'; try { Copy-Item -Path $src -Destination $t -Force } catch { $old = 'bin\WCS_old.exe'; Move-Item -Path $t -Destination $old -Force -ErrorAction SilentlyContinue; Copy-Item -Path $src -Destination $t -Force -ErrorAction SilentlyContinue; }"

rem Remove old legacy binaries if present
if exist "bin\WindowsConfigStudio.exe" del /F /Q "bin\WindowsConfigStudio.exe" >nul 2>&1
if exist "bin\WindowsConfigStudio_Debug.exe" del /F /Q "bin\WindowsConfigStudio_Debug.exe" >nul 2>&1
if exist "build\bin\WindowsConfigStudio_Debug.exe" del /F /Q "build\bin\WindowsConfigStudio_Debug.exe" >nul 2>&1

xcopy /E /I /Y /D "resources" "bin\resources" >nul 2>&1
xcopy /E /I /Y /D "resources" "build\bin\resources" >nul 2>&1

rem If user has WCS.exe or WindowsConfigStudio.exe on Desktop, sync it too
if exist "%USERPROFILE%\Desktop\WCS.exe" (
    copy /Y "bin\WCS.exe" "%USERPROFILE%\Desktop\WCS.exe" >nul 2>&1
)
if exist "%USERPROFILE%\Desktop\WindowsConfigStudio.exe" (
    copy /Y "bin\WCS.exe" "%USERPROFILE%\Desktop\WindowsConfigStudio.exe" >nul 2>&1
)

echo [SUCCESS] Standalone Single-EXE Build finished!
echo   - Output: bin\WCS.exe

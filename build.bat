@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

set "GCC_BIN=C:\Users\Administrator\AppData\Local\Programs\RedPanda-Cpp\mingw64\bin"
set "PATH=%GCC_BIN%;%PATH%"

set "SRC=lyb_webview.cpp"
set "OUT=LYB_Engine2.0.exe"
set "ICON_OBJ=app_icon_mingw.o"
set "WEBVIEW_DLL=WebView2Loader.dll"
set "WEBVIEW_DEF=WebView2Loader.def"
set "WEBVIEW_LIB=libWebView2Loader.a"
set "PTHREAD_DLL=libwinpthread-1.dll"

if not exist "%GCC_BIN%\g++.exe" (
    echo [ERROR] Missing g++.exe at: %GCC_BIN%
    exit /b 1
)

if not exist "%GCC_BIN%\windres.exe" (
    echo [ERROR] Missing windres.exe at: %GCC_BIN%
    exit /b 1
)

if not exist "%GCC_BIN%\gendef.exe" (
    echo [ERROR] Missing gendef.exe at: %GCC_BIN%
    exit /b 1
)

if not exist "%GCC_BIN%\dlltool.exe" (
    echo [ERROR] Missing dlltool.exe at: %GCC_BIN%
    exit /b 1
)

if not exist "%SRC%" (
    echo [ERROR] Missing source file: %SRC%
    exit /b 1
)

if not exist "%WEBVIEW_DLL%" (
    echo [ERROR] Missing %WEBVIEW_DLL% in the current folder.
    exit /b 1
)

if not exist "packages\webview2\build\native\include\WebView2.h" (
    echo [ERROR] Missing WebView2 headers under packages\webview2\build\native\include
    exit /b 1
)

if exist "%WEBVIEW_DEF%" del /f /q "%WEBVIEW_DEF%"
if exist "%WEBVIEW_LIB%" del /f /q "%WEBVIEW_LIB%"
if exist "%ICON_OBJ%" del /f /q "%ICON_OBJ%"

gendef "%WEBVIEW_DLL%"
if errorlevel 1 (
    echo [ERROR] Failed to generate %WEBVIEW_DEF%.
    exit /b 1
)

dlltool -d "%WEBVIEW_DEF%" -l "%WEBVIEW_LIB%" -D "%WEBVIEW_DLL%"
if errorlevel 1 (
    echo [ERROR] Failed to generate %WEBVIEW_LIB%.
    exit /b 1
)

windres app_icon.rc -O coff -o "%ICON_OBJ%"
if errorlevel 1 (
    echo [ERROR] Failed to compile app_icon.rc.
    exit /b 1
)

g++ -std=gnu++17 -O2 -DUNICODE -D_UNICODE -Ipackages\webview2\build\native\include "%SRC%" "%ICON_OBJ%" -o "%OUT%" -mwindows -static-libgcc -static-libstdc++ -L. -lWebView2Loader -lole32 -loleaut32 -luuid -luser32 -lgdi32 -ladvapi32 -lshell32 -lwinmm
if errorlevel 1 (
    echo [ERROR] g++ link failed.
    exit /b 1
)

copy /y "%GCC_BIN%\%PTHREAD_DLL%" "%~dp0%PTHREAD_DLL%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy %PTHREAD_DLL%.
    exit /b 1
)

if not exist "html" mkdir "html"
copy /y "index.html" "html\index.html" >nul

echo Build complete: %OUT%
exit /b 0

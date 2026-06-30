@echo off
setlocal

:: ---- 32-bit (x86) toolchain for original Oblivion + xOBSE ----
:: MSVC compiler (x64 host, x86 target)
set "MSVC_BIN=E:\Programs\visual-studio\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x86"
set "MSVC_INC=E:\Programs\visual-studio\VC\Tools\MSVC\14.50.35717\include"
set "MSVC_LIB=E:\Programs\visual-studio\VC\Tools\MSVC\14.50.35717\lib\x86"

:: Windows SDK (x86)
set "SDK_INC_UM=E:\Windows Kits\10\Include\10.0.26100.0\um"
set "SDK_INC_SHARED=E:\Windows Kits\10\Include\10.0.26100.0\shared"
set "SDK_INC_UCRT=E:\Windows Kits\10\Include\10.0.26100.0\ucrt"
set "SDK_LIB_UM=E:\Windows Kits\10\Lib\10.0.26100.0\um\x86"
set "SDK_LIB_UCRT=E:\Windows Kits\10\Lib\10.0.26100.0\ucrt\x86"

set "PATH=%MSVC_BIN%;%PATH%"
set "INCLUDE=%MSVC_INC%;%SDK_INC_UM%;%SDK_INC_SHARED%;%SDK_INC_UCRT%"
set "LIB=%MSVC_LIB%;%SDK_LIB_UM%;%SDK_LIB_UCRT%"

if not exist build mkdir build

echo Building CappedDamage xOBSE plugin (Win32)...
echo.

:: /MT = static CRT so the plugin has no VC-runtime DLL dependency.
cl.exe /nologo /std:c++17 /O2 /MT /LD /EHsc /Fe:build\cappeddamage.dll /Fo:build\cappeddamage.obj ^
    src\cappeddamage.cpp /link /DLL /DEF:src\exports.def kernel32.lib user32.lib

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED!
    exit /b 1
)

echo.
echo Build successful! Output: build\cappeddamage.dll
echo.

set "PLUGINS=E:\SteamLibrary\steamapps\common\Oblivion\Data\OBSE\Plugins"
if not exist "%PLUGINS%" mkdir "%PLUGINS%"
copy /Y build\cappeddamage.dll "%PLUGINS%\cappeddamage.dll"
echo Deployed to %PLUGINS%
echo.

endlocal

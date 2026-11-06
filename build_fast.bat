@echo off
REM Fast rebuild: only ext_overlay.exe (uses cached lib\libimgui.a).
REM Use this while iterating on src\overlay.cpp. Run build.bat for everything.
cd /d "%~dp0"
REM Fail before compiling if a running overlay locks the output file.
tasklist /FI "IMAGENAME eq ext_overlay.exe" 2>nul | find /I "ext_overlay.exe" >nul
if not errorlevel 1 (
  echo [build] ext_overlay.exe is running - close it, it locks the output file.
  echo [build] taskkill /IM ext_overlay.exe /F  to force it.
  exit /b 2
)
set GCC=C:\Users\adminn\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe
set IMGUI=thirdparty\imgui-master
if not exist lib\libimgui.a (
  echo lib missing, running full build.bat once...
  call build.bat
) else (
  "%GCC%" -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp src\ntwrite.cpp src\ovl\ovl_ui.cpp src\ovl\ovl_cfg.cpp src\ovl\ovl_server.cpp src\ovl\ovl_world.cpp src\ovl\ovl_cache.cpp src\ovl\ovl_esp.cpp src\ovl\ovl_tools.cpp src\ovl\ovl_fly.cpp src\ovl\ovl_stunt.cpp src\ovl\ovl_aim.cpp src\ovl\ovl_tp.cpp src\ovl\ovl_spectate.cpp src\ovl\tab_body.cpp src\ovl\tab_combat.cpp src\ovl\tab_visual.cpp src\ovl\tab_teleport.cpp src\ovl\tab_world.cpp src\ovl\tab_server.cpp src\ovl\tab_tools.cpp src\ovl\tab_misc.cpp src\ovl\tab_stunt.cpp lib\libimgui.a -I%IMGUI% -I%IMGUI%\backends -Isrc -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
)
echo build overlay exit: %ERRORLEVEL%
if exist ext_overlay.exe (
  for /f "delims=" %%H in ('powershell -NoProfile -Command "(Get-FileHash ext_overlay.exe -Algorithm SHA256).Hash.Substring(0,12)"') do (
    for %%F in (ext_overlay.exe) do echo [build] ext_overlay.exe %%~tF sha256=%%H
  )
)
exit /b %ERRORLEVEL%

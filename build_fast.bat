@echo off
REM Fast rebuild: only ext_overlay.exe (uses cached lib\libimgui.a).
REM Use this while iterating on src\overlay.cpp. Run build.bat for everything.
cd /d "%~dp0"
set GCC=C:\Users\adminn\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe
set IMGUI=thirdparty\imgui-master
if not exist lib\libimgui.a (
  echo lib missing, running full build.bat once...
  call build.bat
) else (
  "%GCC%" -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp src\ovl\ovl_ui.cpp src\ovl\tab_body.cpp src\ovl\tab_combat.cpp src\ovl\tab_visual.cpp src\ovl\tab_teleport.cpp src\ovl\tab_world.cpp src\ovl\tab_server.cpp src\ovl\tab_tools.cpp src\ovl\tab_misc.cpp lib\libimgui.a -I%IMGUI% -I%IMGUI%\backends -Isrc -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
)
echo build overlay exit: %ERRORLEVEL%
exit /b %ERRORLEVEL%

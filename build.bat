@echo off
cd /d "%~dp0"
REM A running overlay holds ext_overlay.exe open; the linker then fails with
REM "Permission denied" only after minutes of compiling. Fail fast instead.
tasklist /FI "IMAGENAME eq ext_overlay.exe" 2>nul | find /I "ext_overlay.exe" >nul
if not errorlevel 1 (
  echo [build] ext_overlay.exe is running - close it, it locks the output file.
  echo [build] taskkill /IM ext_overlay.exe /F  to force it.
  exit /b 2
)
set GCC=C:\Users\adminn\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe
"%GCC%" -std=c++17 -O2 -static -o ext_p0.exe src\main.cpp src\offsets.cpp -lwinhttp -lshell32
echo build p0 exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_p1.exe src\p1.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build p1 exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_camwatch.exe src\camwatch.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build camwatch exit: %ERRORLEVEL%
set IMGUI=thirdparty\imgui-master
set AR=%GCC:g++.exe=ar.exe%
REM ImGui rarely changes: build once into a static lib, reuse forever.
REM (Delete lib\libimgui.a to force a rebuild of ImGui itself.)
if not exist lib\libimgui.a (
  echo [imgui] first-time build, one minute...
  if not exist obj mkdir obj
  if not exist lib mkdir lib
  "%GCC%" -std=c++17 -O2 -c -o obj\imgui.o %IMGUI%\imgui.cpp -I%IMGUI%
  "%GCC%" -std=c++17 -O2 -c -o obj\imgui_draw.o %IMGUI%\imgui_draw.cpp -I%IMGUI%
  "%GCC%" -std=c++17 -O2 -c -o obj\imgui_tables.o %IMGUI%\imgui_tables.cpp -I%IMGUI%
  "%GCC%" -std=c++17 -O2 -c -o obj\imgui_widgets.o %IMGUI%\imgui_widgets.cpp -I%IMGUI%
  "%GCC%" -std=c++17 -O2 -c -o obj\imgui_impl_win32.o %IMGUI%\backends\imgui_impl_win32.cpp -I%IMGUI% -I%IMGUI%\backends
  "%GCC%" -std=c++17 -O2 -c -o obj\imgui_impl_dx11.o %IMGUI%\backends\imgui_impl_dx11.cpp -I%IMGUI% -I%IMGUI%\backends
  "%AR%" rcs lib\libimgui.a obj\imgui.o obj\imgui_draw.o obj\imgui_tables.o obj\imgui_widgets.o obj\imgui_impl_win32.o obj\imgui_impl_dx11.o
)
"%GCC%" -std=c++17 -O2 -g -static -o ext_overlay.exe src\overlay.cpp src\offsets.cpp src\ntwrite.cpp src\ovl\ovl_ui.cpp src\ovl\ovl_cfg.cpp src\ovl\ovl_server.cpp src\ovl\ovl_world.cpp src\ovl\ovl_cache.cpp src\ovl\ovl_esp.cpp src\ovl\ovl_tools.cpp src\ovl\ovl_fly.cpp src\ovl\ovl_stunt.cpp src\ovl\ovl_aim.cpp src\ovl\ovl_tp.cpp src\ovl\ovl_spectate.cpp src\ovl\tab_body.cpp src\ovl\tab_combat.cpp src\ovl\tab_visual.cpp src\ovl\tab_teleport.cpp src\ovl\tab_world.cpp src\ovl\tab_server.cpp src\ovl\tab_tools.cpp src\ovl\tab_misc.cpp src\ovl\tab_stunt.cpp lib\libimgui.a -I%IMGUI% -I%IMGUI%\backends -Isrc -lwinhttp -lshell32 -lpsapi -ld3d11 -ld3dcompiler -ldwmapi -lgdi32 -lwinmm
echo build overlay exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_diag.exe src\diag.cpp -ldwmapi
echo build diag exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_winlist.exe src\winlist.cpp
echo build winlist exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_probe.exe src\probe.cpp
echo build probe exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_inputtest.exe src\inputtest.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build inputtest exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_walk.exe src\walk.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build walk exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_espcheck.exe src\espcheck.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build espcheck exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_primcheck.exe src\primcheck.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build primcheck exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_projcheck.exe src\projcheck.cpp src\offsets.cpp src\ntwrite.cpp -Isrc -lwinhttp -lshell32 -lpsapi
echo build projcheck exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_wpmtest.exe src\wpmtest.cpp src\offsets.cpp src\ntwrite.cpp -lwinhttp -lshell32 -lpsapi
echo build wpmtest exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_calib.exe src\calib.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build calib exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_explore.exe src\explore.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build explore exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_dump.exe src\dump.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build dump exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_wssample.exe src\wssample.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build wssample exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_wsfast.exe src\wsfast.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build wsfast exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_watch.exe src\watch.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build watch exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_velprobe.exe src\velprobe.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build velprobe exit: %ERRORLEVEL%
REM dumpdata needs elevation to read an external process (uses wmain -> -municode)
"%GCC%" -std=c++17 -O2 -static -municode -o ext_dumpdata.exe src\dumpdata.cpp -lpsapi
echo build dumpdata exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_ntcheck.exe src\ntcheck.cpp src\offsets.cpp src\ntwrite.cpp -lwinhttp -lshell32 -lpsapi
echo build ntcheck exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_scanfields.exe src\scanfields.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build scanfields exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_jumpchk.exe src\jumpchk.cpp src\offsets.cpp -lwinhttp -lshell32 -lpsapi
echo build jumpchk exit: %ERRORLEVEL%
"%GCC%" -std=c++17 -O2 -static -o ext_scan.exe src\scan.cpp -lpsapi
echo build scan exit: %ERRORLEVEL%
if exist ext_overlay.exe (
  for /f "delims=" %%H in ('powershell -NoProfile -Command "(Get-FileHash ext_overlay.exe -Algorithm SHA256).Hash.Substring(0,12)"') do (
    for %%F in (ext_overlay.exe) do echo [build] ext_overlay.exe %%~tF sha256=%%H
  )
)
exit /b %ERRORLEVEL%

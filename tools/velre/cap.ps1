$ErrorActionPreference = 'Continue'
Set-Location 'C:\Users\adminn\ext_project'
$v = Get-Process velext_v1.0.2 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $v) { 'no velext' | Out-File -Encoding utf8 cap_err.txt; exit 1 }
"pid=$($v.Id)" | Out-File -Encoding utf8 cap_pid.txt
Remove-Item tools\velre\vel_frida.log -ErrorAction SilentlyContinue
# 1) observe velext write path (frida, 12 s)
& node tools\velre\run.js $v.Id 12 tools\velre\vel_frida.log *> cap_node.log
# 2) observe Roblox camera side (read-only, 15 s)
& .\ext_velscan.exe -watch 15 *> vel_watch.log
'done' | Out-File -Encoding utf8 cap_done.txt

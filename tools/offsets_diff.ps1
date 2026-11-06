# offsets_diff.ps1 - so 2 ban offsets.json cua theo, in ra chinh xac field nao doi.
#
# Dung sau moi lan Roblox update: tai dump moi, so voi dump cu da bless, doc
# danh sach thay doi de biet field nao da dich (khong phai do 392 offset).
#
#   powershell -ExecutionPolicy Bypass -File tools\offsets_diff.ps1 `
#       offsets_fixtures\version-OLD.json offsets_fixtures\version-NEW.json
#
# Khong can game, khong can mang, khong build gi.

param(
    [Parameter(Mandatory = $true)][string]$Old,
    [Parameter(Mandatory = $true)][string]$New
)

$ErrorActionPreference = 'Stop'

function Flatten($node, [string]$prefix, $out) {
    if ($null -eq $node) { return }
    if ($node -is [System.Management.Automation.PSCustomObject]) {
        foreach ($p in $node.PSObject.Properties) {
            $path = if ($prefix) { "$prefix.$($p.Name)" } else { $p.Name }
            Flatten $p.Value $path $out
        }
    } else {
        $out[$prefix] = $node
    }
}

function Load($path) {
    if (-not (Test-Path $path)) { throw "khong thay file: $path" }
    $raw = [IO.File]::ReadAllText((Resolve-Path $path), [Text.Encoding]::UTF8)
    $json = $raw | ConvertFrom-Json
    if (-not $json.Offsets) { throw "khong phai dump cua theo (thieu .Offsets): $path" }
    $flat = @{}
    Flatten $json.Offsets '' $flat
    return [pscustomobject]@{
        Path    = $path
        Version = $json.'Roblox Version'
        Dumped  = $json.'Dumped At'
        Flat    = $flat
    }
}

$o = Load $Old
$a = Load $New

Write-Host "old: $($o.Version)  ($($o.Dumped))  $($o.Path)"
Write-Host "new: $($a.Version)  ($($a.Dumped))  $($a.Path)"
Write-Host ''

$keys = @($o.Flat.Keys) + @($a.Flat.Keys) | Sort-Object -Unique
$changed = 0; $added = 0; $removed = 0; $same = 0

# theo's dump contains live pointers that change on every request. They are not
# layout offsets and the tool does not consume them -> never report as drift.
$Ignore = @('Humanoid.PlatformStatePointer')

foreach ($k in $keys) {
    if ($Ignore -contains $k) { continue }
    $hadOld = $o.Flat.ContainsKey($k)
    $hasNew = $a.Flat.ContainsKey($k)
    if (-not $hadOld) {
        Write-Host ("  ADDED    {0} = {1}" -f $k, $a.Flat[$k]) -ForegroundColor Cyan
        ++$added
    } elseif (-not $hasNew) {
        Write-Host ("  REMOVED  {0}   (was {1})" -f $k, $o.Flat[$k]) -ForegroundColor Yellow
        ++$removed
    } elseif ("$($o.Flat[$k])" -ne "$($a.Flat[$k])") {
        $delta = [int64]$a.Flat[$k] - [int64]$o.Flat[$k]
        Write-Host ("  CHANGED  {0}: {1} -> {2}  ({3:+#;-#;0})" -f `
            $k, $o.Flat[$k], $a.Flat[$k], $delta) -ForegroundColor Red
        ++$changed
    } else {
        ++$same
    }
}

Write-Host ''
Write-Host ("tong ket: {0} doi, {1} them, {2} bo, {3} giu nguyen" -f `
    $changed, $added, $removed, $same)

# Version moi ma moi field deu giu nguyen nghia la Roblox chi doi build id,
# khong doi layout: chay -verify la du de biet co dung duoc khong.
if ($changed -eq 0 -and $added -eq 0 -and $removed -eq 0) {
    Write-Host '=> Layout khong doi. Chay: ext_overlay.exe -verify'
} else {
    Write-Host '=> Layout co thay doi. Doc cac dong CHANGED/REMOVED o tren, doi chieu'
    Write-Host '   voi cac cho hardcode trong src (neu con) truoc khi tin dump moi.'
}

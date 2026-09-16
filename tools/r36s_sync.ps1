# R36S card sync (task 6.4) -- the only thing the user runs: put the R36S card in the PC, double-click R36S_SYNC.bat.
#  1. collects the last device run from the card (logs, settings) into docs\device\<date>\ and removes
#     docs\R36S_TEST_REQUIRED.md; a conf\selftest folder left by older builds is collected and deleted
#  2. installs / updates the port: ports\BobrHopper.sh (LF checked), ports\bobrhopper\bobrhopper.aarch64, data\
#     (conf\ with the player's settings stays)
#  3. installs PortMaster's WestonPack runtime if the card's PortMaster lacks it (the launcher's video fallback)
#  4. verifies the copy and flushes the card (it is not ejected: the user keeps it mounted)
# The card is the ArkOS EASYROMS partition: its root is /roms on the device.
param(
    [string]$CardRoot = "",      # a folder standing in for the card (tests); default: the EASYROMS drive
    [string]$DocsRoot = "",      # where docs\device and R36S_TEST_REQUIRED.md live (tests); default: repo docs
    [switch]$NoEject
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $DocsRoot) { $DocsRoot = Join-Path $repo 'docs' }

function Say([string]$msg) { Write-Host $msg }
function Fail([string]$msg) { Write-Host "BLAD: $msg" -ForegroundColor Red; exit 1 }

# --- the card ------------------------------------------------------------------------------------------------
$drive = $null
if ($CardRoot) {
    if (-not (Test-Path -LiteralPath $CardRoot)) { Fail "brak folderu karty $CardRoot" }
} else {
    $vol = Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystemLabel -eq 'EASYROMS' } | Select-Object -First 1
    if (-not $vol) { Fail "nie widze karty R36S (partycja EASYROMS). Wloz karte i uruchom ponownie." }
    $drive = [string]$vol.DriveLetter
    $CardRoot = "${drive}:\"
}
$ports = Join-Path $CardRoot 'ports'
if (-not (Test-Path -LiteralPath $ports)) { Fail "na karcie nie ma folderu ports ($ports)" }
$game = Join-Path $ports 'bobrhopper'
Say "Karta: $CardRoot"

# --- 0. the old name ------------------------------------------------------------------------------------------
# O17: the port used to live in ports\crossyroad with ports\CrossyRoad.sh. Renaming the folder carries the player's
# settings, the best score and the last run's logs across in one move (the logs are renamed too, so step 1 still
# collects them); the old launcher would otherwise stay in the console's Ports menu beside the new one.
$oldGame = Join-Path $ports 'crossyroad'
if ((Test-Path -LiteralPath $oldGame) -and -not (Test-Path -LiteralPath $game)) {
    Rename-Item -LiteralPath $oldGame -NewName 'bobrhopper'
    Say "Zmieniono nazwe folderu na karcie: ports\crossyroad -> ports\bobrhopper (ustawienia i rekord zachowane)."
} elseif (Test-Path -LiteralPath $oldGame) {
    $oldCfg = Join-Path $oldGame 'conf\crossy.cfg'
    $newCfg = Join-Path $game 'conf\crossy.cfg'
    if ((Test-Path -LiteralPath $oldCfg) -and -not (Test-Path -LiteralPath $newCfg)) {
        New-Item -ItemType Directory -Force -Path (Join-Path $game 'conf') | Out-Null
        Copy-Item -LiteralPath $oldCfg -Destination $newCfg -Force
    }
    Remove-Item -LiteralPath $oldGame -Recurse -Force
    Say "Usunieto stary folder ports\crossyroad (ustawienia zachowane)."
}
foreach ($pair in @(@('crossyroad.log', 'bobrhopper.log'), @('crossyroad-launcher.log', 'bobrhopper-launcher.log'))) {
    $stale = Join-Path $game $pair[0]
    if (Test-Path -LiteralPath $stale) { Move-Item -LiteralPath $stale -Destination (Join-Path $game $pair[1]) -Force }
}
$oldLauncher = Join-Path $ports 'CrossyRoad.sh'
if (Test-Path -LiteralPath $oldLauncher) {
    Remove-Item -LiteralPath $oldLauncher -Force
    Say "Usunieto stary launcher ports\CrossyRoad.sh."
}

# --- 1. collect the last device run --------------------------------------------------------------------------
$launcherLog = Join-Path $game 'bobrhopper-launcher.log'
if (Test-Path -LiteralPath $launcherLog) {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HHmm'
    $dest = Join-Path (Join-Path $DocsRoot 'device') $stamp
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    foreach ($name in @('bobrhopper-launcher.log', 'bobrhopper.log')) {
        $src = Join-Path $game $name
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $dest -Force }
    }
    $cfg = Join-Path $game 'conf\crossy.cfg'
    if (Test-Path -LiteralPath $cfg) { Copy-Item -LiteralPath $cfg -Destination $dest -Force }
    $mode = Join-Path $game 'conf\video_mode'
    if (Test-Path -LiteralPath $mode) { Copy-Item -LiteralPath $mode -Destination $dest -Force }
    Say "Zebrano logi z konsoli: $dest"
    $required = Join-Path $DocsRoot 'R36S_TEST_REQUIRED.md'
    if (Test-Path -LiteralPath $required) {
        Remove-Item -LiteralPath $required -Force
        Say "R36S_TEST_REQUIRED.md usuniety, Claude przeanalizuje logi."
    }
}
# older builds ran a self test on the first start; keep its report, free the card
$legacySelftest = Join-Path $game 'conf\selftest'
if (Test-Path -LiteralPath $legacySelftest) {
    $keep = Join-Path (Join-Path $DocsRoot 'device') ('selftest_' + (Get-Date -Format 'yyyy-MM-dd_HHmm'))
    New-Item -ItemType Directory -Force -Path $keep | Out-Null
    Copy-Item -Path (Join-Path $legacySelftest '*') -Destination $keep -Recurse -Force
    Remove-Item -LiteralPath $legacySelftest -Recurse -Force
    Say "Stary self test zabrany do $keep i usuniety z karty."
}

# --- 2. install / update the port ----------------------------------------------------------------------------
$bin = Join-Path $repo 'out\r36s\bobrhopper.aarch64'
$data = Join-Path $repo 'data'
$launcher = Join-Path $repo 'port\BobrHopper.sh'
foreach ($p in @($bin, (Join-Path $data 'manifest.txt'), $launcher)) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "brak $p (zbuduj: sh build/build_r36s.sh bobrhopper, sh build/bake_all.sh)" }
}
$launcherBytes = [System.IO.File]::ReadAllBytes($launcher)
if ($launcherBytes -contains 13) { Fail "port\BobrHopper.sh ma konce linii CRLF - na konsoli nie wystartuje" }

New-Item -ItemType Directory -Force -Path $game | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $ports 'BobrHopper.sh'), $launcherBytes)
Copy-Item -LiteralPath $bin -Destination (Join-Path $game 'bobrhopper.aarch64') -Force
$cardData = Join-Path $game 'data'
if ((Test-Path -LiteralPath $cardData) -and $cardData.EndsWith('bobrhopper\data')) {
    Remove-Item -LiteralPath $cardData -Recurse -Force
}
Copy-Item -LiteralPath $data -Destination $cardData -Recurse -Force
New-Item -ItemType Directory -Force -Path (Join-Path $game 'conf') | Out-Null
foreach ($old in @('bobrhopper-launcher.log', 'bobrhopper.log')) {
    $p = Join-Path $game $old
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
}
Say "Zainstalowano BobrHopper."

# --- 3. WestonPack runtime -----------------------------------------------------------------------------------
$pmLibs = Join-Path $ports 'PortMaster\libs'
$weston = Join-Path $pmLibs 'weston_pkg_0.2.squashfs'
if ((Test-Path -LiteralPath (Join-Path $ports 'PortMaster')) -and -not (Test-Path -LiteralPath $weston)) {
    $sources = @(
        (Join-Path $env:LOCALAPPDATA 'CrossyRoads\tools\weston_pkg_0.2.squashfs'),
        'I:\GITHUB\W_OPEN_SWOS\build\r36s-weston\runtime\weston_pkg_0.2.squashfs'
    )
    $found = $sources | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($found) {
        New-Item -ItemType Directory -Force -Path $pmLibs | Out-Null
        Copy-Item -LiteralPath $found -Destination $weston -Force
        Say "Dograno WestonPack (zapasowy tryb wideo)."
    } else {
        Say "Uwaga: brak WestonPack na karcie i na PC - zapasowy tryb wideo niedostepny."
    }
}

# --- 4. verify, flush---------------------------------------------------------------------------------
$problems = @()
if ((Get-Item -LiteralPath (Join-Path $game 'bobrhopper.aarch64')).Length -ne (Get-Item -LiteralPath $bin).Length) { $problems += 'binarka' }
$srcCount = (Get-ChildItem -LiteralPath $data -Recurse -File).Count
$dstCount = (Get-ChildItem -LiteralPath $cardData -Recurse -File).Count
if ($srcCount -ne $dstCount) { $problems += "data ($dstCount z $srcCount plikow)" }
if ([System.IO.File]::ReadAllBytes((Join-Path $ports 'BobrHopper.sh')) -contains 13) { $problems += 'launcher CRLF' }
if ($problems.Count) { Fail ("kopia niekompletna: " + ($problems -join ', ')) }
Say "Sprawdzono: binarka, $dstCount plikow danych, launcher LF."

# the card stays mounted (the user: no ejecting); only the write cache is flushed
if ($drive) {
    Write-VolumeCache -DriveLetter $drive
    Say "Zapisano na karte."
}
Say "Na konsoli: Ports -> BobrHopper. Wyjscie: Select+Start."
exit 0

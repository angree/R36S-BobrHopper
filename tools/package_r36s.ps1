# Task 6.6: the release package for a manual install (R36S_SYNC.bat is the normal way):
# out\package\BobrHopper-R36S.zip with ports/BobrHopper.sh, ports/bobrhopper/bobrhopper.aarch64,
# ports/bobrhopper/data/... and CZYTAJ.txt -- unpacked onto the card's EASYROMS partition.
# Refuses a stale binary (older than any source file) and a CRLF launcher. Entry names use '/', so Linux tools
# see folders, not file names with backslashes (Compress-Archive in PowerShell 5.1 gets this wrong).
param([string]$Version = "v002", [string]$Out = "")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo "out\package\BobrHopper-R36S-$Version.zip" }
function Fail([string]$msg) { Write-Host "BLAD: $msg" -ForegroundColor Red; exit 1 }

$bin = Join-Path $repo 'out\r36s\bobrhopper.aarch64'
$launcher = Join-Path $repo 'port\BobrHopper.sh'
$data = Join-Path $repo 'data'
foreach ($p in @($bin, $launcher, (Join-Path $data 'manifest.txt'))) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "brak $p" }
}
if ([System.IO.File]::ReadAllBytes($launcher) -contains 13) { Fail "port\BobrHopper.sh ma CRLF" }
$newestSource = Get-ChildItem -LiteralPath (Join-Path $repo 'src'), (Join-Path $repo 'apps') -Recurse -File |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestSource.LastWriteTime -gt (Get-Item -LiteralPath $bin).LastWriteTime) {
    Fail "binarka starsza niz $($newestSource.Name) - przebuduj: sh build/build_r36s.sh bobrhopper"
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$readme = @"
Bóbr Hopper dla R36S (ArkOS) - $Version
===========================

INSTALACJA
1. Wyjmij karte SD z konsoli i wloz do komputera.
2. Otworz partycje EASYROMS (to ta z folderami gier: ports, nes, psx...).
3. Rozpakuj ten ZIP bezposrednio do EASYROMS, tak zeby folder "ports" z ZIP-a polaczyl sie
   z istniejacym folderem "ports". Po rozpakowaniu musza byc:
      EASYROMS\ports\BobrHopper.sh
      EASYROMS\ports\bobrhopper\bobrhopper.aarch64
      EASYROMS\ports\bobrhopper\data\...
4. Bezpiecznie wysun karte, wloz do konsoli i uruchom ja ponownie.
5. W menu: Ports -> BobrHopper. (Jesli pozycji nie widac: Options -> Update/Refresh game list albo restart.)

STEROWANIE
  D-pad         skok (wcisniecie = przysiad, puszczenie = skok)
  A             skok do przodu / start / zagraj ponownie
  Start         pauza
  Select        ustawienia (dzwieki, muzyka, cienie, widok, jezyk, postac)
  Select+Start  wyjscie z gry
  Select+L      licznik FPS

Jesli gra sie nie uruchomi: zapisuje log w EASYROMS\ports\bobrhopper\bobrhopper-launcher.log.
Ustawienia i rekord: EASYROMS\ports\bobrhopper\conf\crossy.cfg.


Bobr Hopper for R36S (ArkOS) - $Version
===========================

INSTALL
1. Take the SD card out of the console and put it in your computer.
2. Open the EASYROMS partition (the one with the game folders: ports, nes, psx...).
3. Extract this ZIP straight into EASYROMS so its "ports" folder merges with the existing "ports" folder:
      EASYROMS\ports\BobrHopper.sh
      EASYROMS\ports\bobrhopper\bobrhopper.aarch64
      EASYROMS\ports\bobrhopper\data\...
4. Eject the card safely, put it back in the console and restart it.
5. Menu: Ports -> BobrHopper. (Not listed? Options -> Update/Refresh game list, or restart.)

CONTROLS
  D-pad         hop (press = crouch, release = hop)
  A             hop forward / start / play again
  Start         pause
  Select        settings (sounds, music, shadows, view, language, character)
  Select+Start  quit
  Select+L      FPS counter

If it does not start, it writes a log to EASYROMS\ports\bobrhopper\bobrhopper-launcher.log.
"@
$zip = [System.IO.Compression.ZipFile]::Open($Out, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $add = {
        param([string]$path, [string]$name)
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $path, $name,
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    & $add $launcher 'ports/BobrHopper.sh'
    & $add $bin 'ports/bobrhopper/bobrhopper.aarch64'
    foreach ($f in Get-ChildItem -LiteralPath $data -Recurse -File) {
        $rel = $f.FullName.Substring($data.Length).TrimStart('\').Replace('\', '/')
        & $add $f.FullName ('ports/bobrhopper/data/' + $rel)
    }
    $entry = $zip.CreateEntry('CZYTAJ.txt')
    $writer = New-Object System.IO.StreamWriter($entry.Open())
    $writer.Write($readme.Replace("`r`n", "`n").Replace("`n", "`r`n"))
    $writer.Dispose()
} finally {
    $zip.Dispose()
}

# verify what was written
$check = [System.IO.Compression.ZipFile]::OpenRead($Out)
try {
    $names = @($check.Entries | ForEach-Object { $_.FullName })
    $dataFiles = (Get-ChildItem -LiteralPath $data -Recurse -File).Count
    if (@($names | Where-Object { $_.Contains('\') }).Count) { Fail "wpisy z backslashem w zipie" }
    if (@($names | Where-Object { $_.StartsWith('ports/bobrhopper/data/') }).Count -ne $dataFiles) { Fail "niepelne dane w zipie" }
    $sh = $check.GetEntry('ports/BobrHopper.sh')
    $ms = New-Object System.IO.MemoryStream
    $s = $sh.Open(); $s.CopyTo($ms); $s.Dispose()
    if ($ms.ToArray() -contains 13) { Fail "launcher w zipie ma CR" }
    if ($check.GetEntry('ports/bobrhopper/bobrhopper.aarch64').Length -ne (Get-Item -LiteralPath $bin).Length) { Fail "binarka w zipie ma zly rozmiar" }
} finally {
    $check.Dispose()
}
$size = [math]::Round((Get-Item -LiteralPath $Out).Length / 1MB, 1)
Write-Host "Paczka: $Out ($size MB, $($names.Count) plikow; launcher LF, $dataFiles plikow danych)"
exit 0

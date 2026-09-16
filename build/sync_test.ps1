# Task 6.4: tools\r36s_sync.ps1 against a fake card (out\check\fakecard) and fake docs (out\check\fakedocs):
# install on an empty card with PortMaster but no WestonPack; a simulated console run (logs, settings, finished
# run, plus a conf\selftest folder left by an older build); a second sync that collects the run, removes
# R36S_TEST_REQUIRED.md, keeps the settings and moves the old self test off the card.
#   powershell -NoProfile -ExecutionPolicy Bypass -File build\sync_test.ps1
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$card = Join-Path $repo 'out\check\fakecard'
$docs = Join-Path $repo 'out\check\fakedocs'
foreach ($p in @($card, $docs)) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } }
New-Item -ItemType Directory -Force -Path (Join-Path $card 'ports\PortMaster') | Out-Null
New-Item -ItemType Directory -Force -Path $docs | Out-Null
Set-Content -LiteralPath (Join-Path $docs 'R36S_TEST_REQUIRED.md') -Value 'test' -Encoding utf8
$sync = Join-Path $repo 'tools\r36s_sync.ps1'
$script:fail = 0
function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what"; $script:fail = 1 }
}
$game = Join-Path $card 'ports\bobrhopper'

Write-Host "sync_test: first sync (install)"
& powershell -NoProfile -ExecutionPolicy Bypass -File $sync -CardRoot $card -DocsRoot $docs | Out-Host
Check "exit code 0" ($LASTEXITCODE -eq 0)
Check "launcher installed without CR" ((Test-Path (Join-Path $card 'ports\BobrHopper.sh')) -and -not ([System.IO.File]::ReadAllBytes((Join-Path $card 'ports\BobrHopper.sh')) -contains 13))
Check "binary installed" (Test-Path (Join-Path $game 'bobrhopper.aarch64'))
Check "data complete" ((Get-ChildItem (Join-Path $game 'data') -Recurse -File).Count -eq (Get-ChildItem (Join-Path $repo 'data') -Recurse -File).Count)
Check "WestonPack installed into PortMaster\libs" (Test-Path (Join-Path $card 'ports\PortMaster\libs\weston_pkg_0.2.squashfs'))
Check "nothing collected yet" (-not (Test-Path (Join-Path $docs 'device')))

Write-Host "sync_test: simulated console run"
New-Item -ItemType Directory -Force -Path (Join-Path $game 'conf\selftest') | Out-Null
Set-Content -LiteralPath (Join-Path $game 'bobrhopper-launcher.log') -Value 'STEP9  done' -Encoding ascii
Set-Content -LiteralPath (Join-Path $game 'bobrhopper.log') -Value 'end: steps=1' -Encoding ascii
Set-Content -LiteralPath (Join-Path $game 'conf\crossy.cfg') -Value 'highscore=42' -Encoding ascii
Set-Content -LiteralPath (Join-Path $game 'conf\video_mode') -Value 'native' -Encoding ascii
Set-Content -LiteralPath (Join-Path $game 'conf\selftest\report.txt') -Value @('bench_full frames=570', 'selftest=ok') -Encoding ascii
Set-Content -LiteralPath (Join-Path $game 'conf\selftest\done') -Value 'ok' -Encoding ascii

Write-Host "sync_test: second sync (collect + update)"
& powershell -NoProfile -ExecutionPolicy Bypass -File $sync -CardRoot $card -DocsRoot $docs | Out-Host
Check "exit code 0" ($LASTEXITCODE -eq 0)
$runs = @(Get-ChildItem (Join-Path $docs 'device') -Directory | Where-Object { $_.Name -notlike 'selftest_*' })
Check "one device run folder" ($runs.Count -eq 1)
if ($runs.Count -eq 1) {
    $d = $runs[0].FullName
    Check "logs and settings collected" ((Test-Path (Join-Path $d 'bobrhopper-launcher.log')) -and (Test-Path (Join-Path $d 'crossy.cfg')) -and (Test-Path (Join-Path $d 'video_mode')))
}
$old = @(Get-ChildItem (Join-Path $docs 'device') -Directory -Filter 'selftest_*')
Check "old self test report kept in docs" (($old.Count -eq 1) -and (Test-Path (Join-Path $old[0].FullName 'report.txt')))
Check "old self test removed from the card" (-not (Test-Path (Join-Path $game 'conf\selftest')))
Check "R36S_TEST_REQUIRED.md removed" (-not (Test-Path (Join-Path $docs 'R36S_TEST_REQUIRED.md')))
Check "settings kept on the card" ((Get-Content (Join-Path $game 'conf\crossy.cfg')) -contains 'highscore=42')
Check "old logs cleared for the next run" (-not (Test-Path (Join-Path $game 'bobrhopper-launcher.log')))

foreach ($p in @($card, $docs)) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } }
if ($script:fail) { Write-Host "sync_test: FAILED"; exit 1 }
Write-Host "sync_test: OK"
exit 0

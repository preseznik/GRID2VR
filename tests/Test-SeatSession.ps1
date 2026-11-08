$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Seat-Session.ps1')
function Require([bool]$value,[string]$reason) {if(!$value){throw $reason}}
function Get-Process { } # Only the owned fixture below is read or written.
$fixture=Join-Path $root ('build/seat-session-test-'+[guid]::NewGuid().ToString('N'))
try {
    foreach($name in @('tools/data','build/win32','artifacts','run','disabled')) {[IO.Directory]::CreateDirectory((Join-Path $fixture $name)) | Out-Null}
    foreach($name in @('seat_session.py','session_config.py','game_executable.py','cockpit_dlc.py')) {Copy-Item -LiteralPath (Join-Path $root "tools/$name") -Destination (Join-Path $fixture "tools/$name")}
    foreach($name in @('cockpit-assets.json','cockpit-dlc.json')) {Copy-Item -LiteralPath (Join-Path $root "tools/data/$name") -Destination (Join-Path $fixture "tools/data/$name")}
    [IO.File]::WriteAllText((Join-Path $fixture 'build/win32/CMakeCache.txt'),('Python3_EXECUTABLE:FILEPATH='+(Get-Grid2Python $root)))
    $seats=Join-Path $fixture 'artifacts/seat-positions.json';$run=Join-Path $fixture 'run'
    [IO.File]::WriteAllText($seats,'{"Version":1,"ApplyToDesktop":true,"InGameSeat":true,"DesktopFov":10,"Cars":{"s20":{"Height":10,"Forward":2,"Right":3},"pag":{"Height":4}}}')
    $request=New-SeatSessionRequest $fixture $run $true $true 10 $false
    Require $request.enabled 'Legacy/default in-game menu did not prepare.'
    $requestPath=Join-Path $run 'seat-menu.request';$hash=(Get-FileHash -LiteralPath $requestPath).Hash.ToLowerInvariant()
    $value=Get-Content -LiteralPath $seats -Raw | ConvertFrom-Json -AsHashtable
    $value.Cars.pag.Height=30;$value.FutureSetting='retain';$value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $seats
    [IO.File]::WriteAllText((Join-Path $run 'seat-menu.commit'),"grid2vr-seat-commit-v1 $hash 20`ns20 -12 5 -3`n")
    Save-SeatSessionCommit $fixture $run
    $saved=Get-Content -LiteralPath $seats -Raw | ConvertFrom-Json
    Require ($saved.Cars.s20.Height -eq -12 -and $saved.Cars.s20.Forward -eq 5 -and $saved.Cars.s20.Right -eq -3 -and $saved.DesktopFov -eq 20) 'Session commit values were lost.'
    Require ($saved.Cars.pag.Height -eq 30 -and $saved.FutureSetting -eq 'retain' -and $saved.ApplyToDesktop) 'An unrelated preference was overwritten.'
    Require (Test-Path -LiteralPath (Join-Path $run 'seat-commit-applied.json')) 'Missing seat commit receipt.'
    $value.InGameSeat=$false;$value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $seats
    $disabled=New-SeatSessionRequest $fixture (Join-Path $fixture 'disabled') $false $true 0 $false
    Require (!$disabled.enabled -and !(Test-Path -LiteralPath (Join-Path $fixture 'disabled/seat-menu.request'))) 'Disabling the tool still armed it.'
    Write-Output 'Seat session request, deferred persistence, unrelated edits, receipt and opt-out passed.'
} finally {
    $full=[IO.Path]::GetFullPath($fixture)
    if($full.StartsWith((Join-Path $root 'build/seat-session-test-'),[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) {Remove-Item -LiteralPath $full -Recurse -Force}
}

param([Parameter(Mandatory)][string]$PackageDirectory,[switch]$SkipInstall,[switch]$KeepInstalled,[switch]$StageOnly)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$package=[IO.Path]::GetFullPath($PackageDirectory)
$test=Join-Path $root 'build/distribution-verification'
$game=Join-Path $test 'game with spaces'
$userBase=Join-Path $test 'user-data'
$originalGame='E:\SteamLibrary\steamapps\common\grid 2'
New-Item -ItemType Directory -Force -Path $game,$userBase | Out-Null
$env:GRID2VR_USER_BASE=$userBase
$env:DOTNET_MULTILEVEL_LOOKUP='0'
$env:PYTHONNOUSERSITE='1'
function Copy-GameFile([string]$Relative) {
    $target=Join-Path $game $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    if(!(Test-Path -LiteralPath $target)) { Copy-Item -LiteralPath (Join-Path $originalGame $Relative) -Destination $target }
}
foreach($name in @('grid2.exe','grid2_avx.exe','bink2w32.dll','cutscene/intro_buildup.xml','system/flow.bin','system/states.bin','frontend/databases/overlayPersistent.pssg','tracks/generic/ornament_system_settings.xml')) {Copy-GameFile $name}
$catalog=Get-Content -LiteralPath (Join-Path $root 'tools/data/cockpit-assets.json') -Raw | ConvertFrom-Json
foreach($car in $catalog.cars) {Copy-GameFile "cars/models/$($car.car)/cameras.xml"}
Copy-GameFile 'cars/models/s20/s20_highLOD.pssg'
New-Item -ItemType Directory -Force -Path (Join-Path $game 'cars/models/s20/livery_user') | Out-Null
foreach($name in @('track.jpk','route_objects.pssg','progress_track.xml')) {Copy-GameFile "tracks/locations/circuits/brands_hatch/route_3/$name"}
$original=@{};Get-ChildItem -LiteralPath $game -File -Recurse | Where-Object {$_.FullName -notlike "$game\GRID2VR\*" -and $_.Name -notin @('GRID2VR.exe','Start-GRID2VR.cmd')} | ForEach-Object {$original[$_.FullName]=(Get-FileHash -LiteralPath $_.FullName).Hash}
function Run([string]$Exe,[string[]]$Arguments,[string]$Name,[switch]$ExpectFailure) {
    $out=Join-Path $test ($Name+'.txt');$err=Join-Path $test ($Name+'-error.txt')
    $info=[Diagnostics.ProcessStartInfo]::new($Exe)
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($arg in $Arguments){$info.ArgumentList.Add($arg)}
    $process=[Diagnostics.Process]::Start($info)
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if(!$process.WaitForExit(300000)) { throw "$Name exceeded five minutes; owned process $($process.Id) retained for inspection." }
    [IO.File]::WriteAllText($out,$stdout.GetAwaiter().GetResult());[IO.File]::WriteAllText($err,$stderr.GetAwaiter().GetResult())
    if($ExpectFailure) { if($process.ExitCode -eq 0) {throw "$Name incorrectly accepted the unsafe fixture."} }
    elseif($process.ExitCode -ne 0) { throw "$Name failed with $($process.ExitCode): $(Get-Content -LiteralPath $err -Raw)" }
    $process.Dispose()
}
$packageVersion=(Get-Content -LiteralPath (Join-Path $package 'stage/GRID2VR/package.json') -Raw | ConvertFrom-Json).version
$installer=Join-Path $package ("GRID2VR-$packageVersion-Setup.exe")
if($StageOnly) { Copy-Item -Path (Join-Path $package 'stage/*') -Destination $game -Recurse -Force }
elseif(!$SkipInstall){Run $installer @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',"/DIR=$game","/LOG=$(Join-Path $test 'install.log')",'/TASKS=') 'install'}
$launcher=Join-Path $game 'GRID2VR.exe';$mod=Join-Path $game 'GRID2VR'
Run $launcher @('--verify-package','--game',$game,'--error-log',(Join-Path $test 'verify-error.txt')) 'verify'
Run $launcher @('--setup','--game',$game,'--error-log',(Join-Path $test 'setup-error.txt')) 'setup'
. (Join-Path $mod 'tools/Paths.ps1')
$data=Get-Grid2DataRoot $mod
# Import a disposable copy of actual preferences; never ship them in the installer.
if(!(Test-Path -LiteralPath (Join-Path $data 'launcher-settings.json'))) {
    $source=Join-Path $test 'import';New-Item -ItemType Directory -Force -Path $source | Out-Null
    foreach($name in @('launcher-settings.json','launcher-window.json','driving-controls.json','seat-positions.json','update-preferences.json')) {
        $path=Join-Path $root "artifacts/$name";if(Test-Path -LiteralPath $path){Copy-Item -LiteralPath $path -Destination (Join-Path $source $name) -Force}
    }
    Run $launcher @('--migrate-from',$source,'--game',$game,'--error-log',(Join-Path $test 'import-error.txt')) 'migration'
}
$preserved=@{};foreach($name in @('launcher-settings.json','launcher-window.json','driving-controls.json','seat-positions.json','update-preferences.json')){$path=Join-Path $data $name;if(Test-Path -LiteralPath $path){$preserved[$path]=(Get-FileHash -LiteralPath $path).Hash}}
$shell=Join-Path $mod 'payload/powershell/pwsh.exe'
Run (Join-Path $mod 'payload/python/python.exe') @('-B','-c',"import sys,importlib;sys.path.insert(0,sys.argv[1]);from pathlib import Path;from PIL import Image;import numpy;[importlib.import_module(p.stem) for p in Path(sys.argv[1]).glob('analyze_*.py')];print('All diagnostic modules imported')",(Join-Path $mod 'tools')) 'python-dependencies'
$recenter=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('{"Action":"Recenter","Device":"Xbox","DeviceId":"xinput:0","DeviceType":"xinput_pad","Input":"xinput_button_back"}'))
foreach($mode in @('Normal','Practice','Race')) {
    Run $shell @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $mod 'tools/Start-UserSession.ps1'),'-Preflight','-PreviewVrFixes','-Mode','VR','-LaunchMode',$mode,'-Car','s20','-Track','circuits/brands_hatch/route_3','-RenderScale','300','-HeadsetScale','200','-VrMsaa','4xmsaa','-CockpitMode','HeadOnly','-SkipIntro','-RecenterDeviceBase64',$recenter) ('preflight-'+$mode)
}
Run $shell @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $mod 'tools/Start-UserSession.ps1'),'-Preflight','-Mode','Desktop','-LaunchMode','Practice','-Car','s20','-Track','circuits/brands_hatch/route_3','-RenderScale','300','-HeadsetScale','200','-DesktopMsaa','4xmsaa') 'preflight-desktop'
Run $shell @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'tests/Test-InstalledRecovery.ps1'),'-ModRoot',$mod) 'recovery'
# Refusals must leave foreign files and interrupted-session receipts intact.
$foreign=Join-Path $game 'd3d11.dll'
[IO.File]::WriteAllText($foreign,'foreign proxy fixture')
try {
    Run $launcher @('--check-install','--game',$game,'--error-log',(Join-Path $test 'foreign-proxy-refused.txt')) 'reject-foreign-proxy' -ExpectFailure
    if([IO.File]::ReadAllText($foreign) -ne 'foreign proxy fixture') {throw 'Foreign proxy changed.'}
} finally {Remove-Item -LiteralPath $foreign}
$pending=Join-Path $data 'session-pending.json'
[IO.File]::WriteAllText($pending,'{}')
try {
    Run $launcher @('--check-install','--game',$game,'--error-log',(Join-Path $test 'pending-refused.txt')) 'reject-pending' -ExpectFailure
    if([IO.File]::ReadAllText($pending) -ne '{}') {throw 'Recovery receipt changed.'}
} finally {Remove-Item -LiteralPath $pending}
$readme=Join-Path $mod 'README.md';$readmeBytes=[IO.File]::ReadAllBytes($readme)
try {
    [IO.File]::AppendAllText($readme,'Changed fixture')
    Run $launcher @('--check-install','--game',$game,'--error-log',(Join-Path $test 'tampered-refused.txt')) 'reject-tampered-package' -ExpectFailure
} finally {[IO.File]::WriteAllBytes($readme,$readmeBytes)}
$invalid=Join-Path $test 'not a game';New-Item -ItemType Directory -Force -Path $invalid | Out-Null
[IO.File]::WriteAllText((Join-Path $invalid 'grid2.exe'),'unsupported fixture')
Run $launcher @('--check-install','--game',$invalid,'--error-log',(Join-Path $test 'invalid-game-refused.txt')) 'reject-invalid-game' -ExpectFailure
# Upgrade the same candidate: compare immutable game files and personal data afterward.
if($StageOnly){Write-Output 'Relocated setup and all launch preflights passed.';return}
Run $installer @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER',"/DIR=$game","/LOG=$(Join-Path $test 'upgrade.log')",'/TASKS=') 'upgrade'
Run $launcher @('--verify-package','--game',$game) 'verify-upgrade'
foreach($path in $original.Keys){if((Get-FileHash -LiteralPath $path).Hash -ne $original[$path]){throw "Game fixture changed: $path"}}
foreach($path in $preserved.Keys){if((Get-FileHash -LiteralPath $path).Hash -ne $preserved[$path]){throw "User file changed: $path"}}
if(!$KeepInstalled) {
    $uninstaller=Join-Path $mod 'uninstall/unins000.exe'
    Run $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/LOG=$(Join-Path $test 'uninstall.log')") 'uninstall'
    if(Test-Path -LiteralPath $launcher){throw 'Launcher remains after uninstall.'}
    foreach($path in $original.Keys){if((Get-FileHash -LiteralPath $path).Hash -ne $original[$path]){throw "Uninstall changed game fixture: $path"}}
    foreach($path in $preserved.Keys){if((Get-FileHash -LiteralPath $path).Hash -ne $preserved[$path]){throw "Uninstall changed user file: $path"}}
}
@{passed=$true;package=$package;gameFixture=$game;gameFiles=$original.Count;userFiles=$preserved.Count;installed=$true;upgraded=$true;uninstalled=(!$KeepInstalled);preflightModes=@('Normal VR','Practice VR','Race VR','Practice Desktop');renderScale=300;headsetScale=200;msaa='4xmsaa';gameLaunched=$false;headsetUsed=$false} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $test 'verification.json') -Encoding utf8
Write-Output 'Installer, setup, all launch preflights, upgrade and preservation checks passed.'

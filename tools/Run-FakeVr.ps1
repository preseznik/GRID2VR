# One unattended VR session on this PC through SteamVR's virtual headset (ProJict a86791d3):
# native AI driving and the bounded flicker capture of Run-AutomatedFlicker.ps1 in Headset
# mode, with the real session's visibility path and Faster stereo as users run it. SteamVR is
# switched for this run only; announce it in #grid2vr first. Evidence holds the run log,
# the original steamvr.vrsettings and run.json with the restore checks.
param(
    [Parameter(Mandatory)][string]$Evidence,
    [string]$Car='z28',
    [string]$Track='circuits/brands_hatch/route_2',
    [ValidateSet('benchmark','attract')][string]$PlaybackMode='benchmark',
    [ValidateSet('Off','Replace')][string]$NativeLeftEye='Replace',
    [ValidateSet(25,50,75,100,125,150,175,200)][int]$HeadsetScale=100,
    [ValidateRange(1,600)][int]$AfterPairs=480,
    [ValidateRange(1,300)][int]$CaptureAfterSeconds=35,
    [ValidateRange(40,600)][int]$TimeoutSeconds=330,
    # Steam holds the launch on VR prompts the virtual headset never answers (~60 s).
    [ValidateRange(60,600)][int]$GameStartSeconds=180,
    [switch]$CaptureSignTransition,
    # Sweep the motionless virtual headset (25 degrees yaw, 8 pitch, 8 s period) for flicker
    # that only shows while looking around.
    [switch]$HeadMotion,
    [ValidateRange(640,4096)][int]$Width=2016,
    [ValidateRange(640,4096)][int]$Height=2240,
    [string]$DiagnosticProxy=(Join-Path $PSScriptRoot '../build/win32/bin/Release/d3d11.dll'),
    # Switch SteamVR and restore it without starting anything: checks the transaction.
    [switch]$SettingsOnly
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Fake-Vr.ps1')
Add-Type -AssemblyName System.Windows.Forms
if([System.Windows.Forms.SystemInformation]::TerminalServerSession) { throw 'Remote Desktop session: run this at the console.' }
if(Get-Process grid2,grid2_avx,GRID2VR -ErrorAction SilentlyContinue) { throw 'GRID 2 or the GRID2VR launcher is running.' }
if(Test-FakeVrSteamVrRunning) { throw 'SteamVR is running (a real headset may be in use). Close it first.' }
if(Test-Path -LiteralPath $Evidence) { throw "Evidence folder exists: $Evidence" }
if(!$SettingsOnly -and !(Test-FakeVrRenderer $DiagnosticProxy)) { throw "This renderer refuses SteamVR's virtual headset (no adapter fix): $DiagnosticProxy" }
if($HeadMotion -and !(Test-FakeVrRenderer $DiagnosticProxy -HeadMotion)) { throw "This renderer has no fake-VR head motion: $DiagnosticProxy" }
$vrSettings=Get-FakeVrSettingsPath
New-Item -ItemType Directory -Path $Evidence | Out-Null
$original=Join-Path $Evidence 'steamvr.vrsettings.original'
Copy-Item -LiteralPath $vrSettings -Destination $original
$hash=(Get-FileHash -LiteralPath $vrSettings).Hash
$result=[ordered]@{schema=1;settings=$vrSettings;settingsSha256=$hash;headset="${Width}x$Height";settingsOnly=[bool]$SettingsOnly
    car=$Car;track=$Track;nativeLeftEye=$NativeLeftEye;headMotion=[bool]$HeadMotion;proxy=$DiagnosticProxy}
try {
    Set-FakeVrNullDriver $vrSettings $Width $Height
    $applied=Get-Content -LiteralPath $vrSettings -Raw | ConvertFrom-Json -AsHashtable
    $result.nullDriverApplied=$applied['steamvr']['forcedDriver'] -eq 'null' -and $applied['power']['pauseCompositorOnStandby'] -eq $false
    if(!$SettingsOnly) {
        $result.steamVrStarted=Start-FakeVrSteamVr
        if(!$result.steamVrStarted) { throw 'SteamVR did not start with the virtual headset within 90 s.' }
        # Recorded for every run; head motion stops here, before GRID 2 starts, unless
        # SteamVR reports the null driver the renderer will accept.
        $result.headsetName=Get-FakeVrHeadsetName (Join-Path (Split-Path $DiagnosticProxy -Parent) 'xr_runtime_probe.exe')
        if($HeadMotion -and !(Test-FakeVrHeadsetName $result.headsetName)) { throw "Head motion needs SteamVR's virtual headset; SteamVR reports: $($result.headsetName)" }
        $run=@{Mode='Headset';XrRuntime='SteamVR';Car=$Car;Track=$Track;PlaybackMode=$PlaybackMode;NativeLeftEye=$NativeLeftEye
            HeadsetScale=$HeadsetScale;AfterPairs=$AfterPairs;CaptureAfterSeconds=$CaptureAfterSeconds;TimeoutSeconds=$TimeoutSeconds;GameStartSeconds=$GameStartSeconds
            CaptureSignTransition=[bool]$CaptureSignTransition;HeadMotion=[bool]$HeadMotion;DiagnosticProxy=(Resolve-Path -LiteralPath $DiagnosticProxy).Path}
        $output=& (Join-Path $PSScriptRoot 'Run-AutomatedFlicker.ps1') @run *>&1 | Tee-Object -FilePath (Join-Path $Evidence 'run.log')
        $result.diagnosticEvidence=@($output | Where-Object { "$_" -like 'Diagnostic evidence: *' } | ForEach-Object { "$_".Substring(21) }) | Select-Object -First 1
    }
}
finally {
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { $result.gameRunningAtRestore=$true }
    $result.steamVrStopped=Stop-FakeVrSteamVr
    Copy-Item -LiteralPath $original -Destination $vrSettings -Force
    $result.steamVrRestored=(Get-FileHash -LiteralPath $vrSettings).Hash -eq $hash
    $result | ConvertTo-Json -Depth 4 | Tee-Object -FilePath (Join-Path $Evidence 'run.json')
}

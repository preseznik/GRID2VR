# SteamVR's built-in null (virtual) headset for unattended agent VR runs on this PC
# (ProJict a86791d3, as DiRT2VR #a3887708). Only steamvr.vrsettings changes, and
# Run-FakeVr.ps1 restores it byte for byte after SteamVR has exited.
. (Join-Path $PSScriptRoot 'Paths.ps1')
$script:FakeVrSteamVr=@('vrserver','vrmonitor','vrcompositor','vrdashboard','vrwebhelper','vrstartup')

function Get-FakeVrSettingsPath { return Join-Path (Get-Grid2SteamRoot) 'config/steamvr.vrsettings' }

function Set-FakeVrNullDriver([string]$Path,[int]$Width=2016,[int]$Height=2240) {
    $settings=Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
    if(!$settings.ContainsKey('steamvr')) { $settings['steamvr']=[ordered]@{} }
    $settings['steamvr']['forcedDriver']='null'
    $settings['steamvr']['activateMultipleDrivers']=$true
    # The motionless virtual headset otherwise enters standby after 5 s: the compositor
    # pauses, the session never becomes visible and the game submits no frames.
    if(!$settings.ContainsKey('power')) { $settings['power']=[ordered]@{} }
    $settings['power']['turnOffScreensTimeout']=86400.0
    $settings['power']['pauseCompositorOnStandby']=$false
    # A small mirror window in the corner instead of the driver's 2160x1200 default.
    $settings['driver_null']=[ordered]@{enable=$true;serialNumber='GRID2VR lab';modelNumber='Null';windowX=0;windowY=0
        windowWidth=640;windowHeight=360;renderWidth=$Width;renderHeight=$Height;secondsFromVsyncToPhotons=0.01111111;displayFrequency=90.0}
    $settings | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Path -Encoding utf8
}

function Test-FakeVrSteamVrRunning { return [bool](Get-Process $script:FakeVrSteamVr -ErrorAction SilentlyContinue) }

# SteamVR rewrites its settings on exit, so the restore waits until every process is gone.
function Stop-FakeVrSteamVr {
    Get-Process $script:FakeVrSteamVr -ErrorAction SilentlyContinue | Stop-Process -Force
    $deadline=(Get-Date).AddSeconds(30)
    while((Get-Date) -lt $deadline -and (Test-FakeVrSteamVrRunning)) { Start-Sleep -Milliseconds 500 }
    return !(Test-FakeVrSteamVrRunning)
}

# The renderer must accept SteamVR's virtual headset, which names no graphics adapter.
function Test-FakeVrRenderer([string]$Path) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and
        [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path)).Contains("runtime names no graphics adapter; using the game's")
}

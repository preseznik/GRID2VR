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
    # GRID2VR renders stereo only with a focused session. SteamVR Home and the dashboard
    # (which also shows Steam's launch prompts in the headset) would keep the focus.
    $settings['steamvr']['enableHomeApp']=$false
    if(!$settings.ContainsKey('dashboard')) { $settings['dashboard']=[ordered]@{} }
    $settings['dashboard']['enableDashboard']=$false
    $settings['dashboard']['autoShowGameTheater']=$false
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

# GRID2VR's SteamVR theater step (steamvr_theater.py) needs a running SteamVR, so the
# virtual headset is started before the session, from the SteamVR install the launcher uses.
function Start-FakeVrSteamVr {
    . (Join-Path $PSScriptRoot 'OpenXR-Runtime.ps1')
    $startup=Join-Path (Split-Path (Get-Grid2VrRuntimeManifest 'SteamVR') -Parent) 'bin/win64/vrstartup.exe'
    if(!(Test-Path -LiteralPath $startup -PathType Leaf)) { throw "SteamVR is not installed where the launcher looks: $startup" }
    Start-Process -FilePath $startup | Out-Null
    $deadline=(Get-Date).AddSeconds(90)
    while((Get-Date) -lt $deadline -and !((Get-Process vrserver -ErrorAction SilentlyContinue) -and (Get-Process vrcompositor -ErrorAction SilentlyContinue))) {
        Start-Sleep -Milliseconds 500
    }
    if(!((Get-Process vrserver -ErrorAction SilentlyContinue) -and (Get-Process vrcompositor -ErrorAction SilentlyContinue))) { return $false }
    Start-Sleep -Seconds 5
    return $true
}

# SteamVR rewrites its settings on exit, so the restore waits until every process is gone.
function Stop-FakeVrSteamVr {
    Get-Process $script:FakeVrSteamVr -ErrorAction SilentlyContinue | Stop-Process -Force
    $deadline=(Get-Date).AddSeconds(30)
    while((Get-Date) -lt $deadline -and (Test-FakeVrSteamVrRunning)) { Start-Sleep -Milliseconds 500 }
    return !(Test-FakeVrSteamVrRunning)
}

# SteamVR's OpenXR system name for the running headset, from the renderer's own runtime
# probe (the launcher's headset query): "SteamVR/OpenXR : <tracking system>".
function Get-FakeVrHeadsetName([string]$Probe) {
    . (Join-Path $PSScriptRoot 'OpenXR-Runtime.ps1')
    if(!(Test-Path -LiteralPath $Probe -PathType Leaf)) { return $null }
    $start=[Diagnostics.ProcessStartInfo]::new($Probe)
    $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
    $start.Environment['XR_RUNTIME_JSON']=Get-Grid2VrRuntimeManifest 'SteamVR'
    $process=[Diagnostics.Process]::Start($start)
    $output=$process.StandardOutput.ReadToEndAsync();$null=$process.StandardError.ReadToEndAsync()
    if(!$process.WaitForExit(15000)) { $process.Kill($true);return $null }
    if($output.GetAwaiter().GetResult() -match '(?m)^headset=(.+?) orientation_tracking=') { return $Matches[1] }
    return $null
}

# Mirrors headmotion::VirtualHeadset in src/head_motion.h: only SteamVR's built-in null
# driver gets the diagnostic head motion.
function Test-FakeVrHeadsetName([string]$Name) { return "$Name" -match '^SteamVR/[^:]*:\s*null\s*$' }

# The renderer must accept SteamVR's virtual headset, which names no graphics adapter,
# and for -HeadMotion it must know the diagnostic sweep (head-motion.request).
function Test-FakeVrRenderer([string]$Path,[switch]$HeadMotion) {
    if(!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $text=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path))
    return $text.Contains("runtime names no graphics adapter; using the game's") -and
        (!$HeadMotion -or $text.Contains('head_motion_requires_virtual_headset'))
}

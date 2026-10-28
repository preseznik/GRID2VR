# Explicit prototype entry; saved launcher preferences are read, never changed.
# Select the normal bonnet camera with the user's camera-cycle control.
param(
    [ValidateSet('Desktop','VR')][string]$Mode='Desktop',
    [ValidateSet('HeadOnly','HideDriver')][string]$DriverVisibility='HeadOnly',
    [ValidateSet('Practice','Normal')][string]$LaunchMode='Practice',
    [switch]$Preflight
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'User-Session.ps1')
$settings=Read-SessionJson (Join-Path $root 'artifacts/launcher-settings.json')
$graphics=$settings.Graphics
if($graphics.RenderScale -ne 300 -or $graphics.HeadsetScale -ne 200 -or $graphics.VrMsaa -ne '4xmsaa') {
    throw 'Saved VR settings differ from the requested 300% render / 200% headset / 4x MSAA test. Nothing launched.'
}
$hidden=0
foreach($flag in @(@('HudGauges',1),@('HudLapTime',2),@('HudPosition',4),@('HudMap',8),@('HudProgress',16))) {
    if(!$graphics.($flag[0])) { $hidden=$hidden -bor $flag[1] }
}
$parameters=@{
    Mode=$Mode;LaunchMode=$LaunchMode;Car='s20';Track=$settings.Track;Laps=$settings.Laps
    XrRuntime=$settings.Runtime;RenderScale=$graphics.RenderScale;HeadsetScale=$graphics.HeadsetScale
    DesktopVSync=[int][bool]$graphics.DesktopVSync;DesktopMsaa=$graphics.DesktopMsaa;VrMsaa=$graphics.VrMsaa
    DesktopBorderless=$(if($null -ne $graphics.DesktopBorderless){[int][bool]$graphics.DesktopBorderless}else{1})
    TreeDetail=$graphics.TreeDetail;ObjectDetail=$graphics.ObjectDetail;CrowdDetail=$graphics.CrowdDetail;TextureFiltering=$graphics.TextureFiltering
    HudDistance=$graphics.HudDistance;HudFollowView=[int][bool]$graphics.HudFollowView;HudHiddenElements=$hidden
    VrReplayCameras=[int][bool]$settings.VrReplayCameras;VrGarageMenus=[int][bool]$settings.VrGarageMenus
    RecenterKey=$settings.RecenterKey;RecenterModifiers=$settings.RecenterModifiers
    SkipIntro=[bool]$settings.SkipIntro;DiagnosticLogging=[bool]$settings.DiagnosticLogging
    PerformanceDisplay=[bool]$settings.PerformanceDisplay;PreviewVrFixes=($Mode -eq 'VR' -and [bool]$settings.PreviewVrFixes)
    DrivingControls=(Join-Path $root 'artifacts/driving-controls.xml');CockpitS2000=$DriverVisibility;Preflight=[bool]$Preflight
}
& (Join-Path $PSScriptRoot 'Start-UserSession.ps1') @parameters

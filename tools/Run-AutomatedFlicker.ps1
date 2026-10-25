# Native AI driving + the existing same-frame stereo renderer; no user interaction.
param(
    [ValidateSet('Simulated','Headset')][string]$Mode='Simulated',
    [ValidateSet('VDXR','SteamVR')][string]$XrRuntime='VDXR',
    [ValidateSet('benchmark','attract')][string]$PlaybackMode='benchmark',
    [string]$Car='z28',
    [string]$Track='circuits/brands_hatch/route_2',
    [ValidateRange(1,20)][int]$Laps=1,
    [ValidateSet(25,50,75,100,125,150,175,200)][int]$HeadsetScale=150,
    [ValidateRange(1,600)][int]$AfterPairs=480,
    [ValidateRange(0,180)][int]$ObservationSeconds=0,
    [ValidateRange(40,600)][int]$TimeoutSeconds=120,
    [ValidateRange(1,300)][int]$CaptureAfterSeconds=35,
    [switch]$CycleCamera,
    [switch]$CaptureSignTransition,
    [switch]$NativeVisibilityControl,
    [switch]$TrackedVisibility,
    [switch]$MirrorPreview,
    [switch]$VisibilityCapture,
    [switch]$ScreenUiStereo,
    [ValidateSet('Off','Observe','Expand')][string]$InstanceCapacity='Off',
    [string]$RecordedViewsFile='',
    [switch]$Preflight,
    [string]$DiagnosticProxy=(Join-Path $PSScriptRoot '../build/flicker-win32/bin/RelWithDebInfo/d3d11.dll')
)
$ErrorActionPreference='Stop'
if($ObservationSeconds -and ($Mode -ne 'Simulated' -or !$TrackedVisibility -or !$VisibilityCapture -or $Preflight -or $CaptureSignTransition -or $CaptureAfterSeconds+$ObservationSeconds+20 -ge $TimeoutSeconds)) { throw 'Extended observation requires bounded simulated tracked visibility with reduced capture and enough timeout.' }
if($InstanceCapacity -ne 'Off' -and (!$TrackedVisibility -or !$VisibilityCapture)) { throw 'Instance-capacity diagnostics require tracked visibility and reduced capture.' }
if($VisibilityCapture -and ((!$TrackedVisibility -and !$NativeVisibilityControl) -or $CaptureSignTransition)) { throw 'Visibility-only capture requires a tracked or native visibility diagnostic without sign-target capture.' }
if($MirrorPreview -and !$TrackedVisibility) { throw 'Mirror preview requires the isolated tracked-visibility diagnostic.' }
if($TrackedVisibility -and ($NativeVisibilityControl -or $Mode -ne 'Simulated' -or $Preflight)) { throw 'Tracked visibility requires an exclusive bounded unattended simulated AI run.' }
if($NativeVisibilityControl -and ($Mode -ne 'Simulated' -or $Preflight)) { throw 'Native-visibility control requires a bounded unattended simulated AI run.' }
if($CaptureSignTransition) {
    if($Mode -ne 'Simulated' -or $Preflight) { throw 'Sign-transition capture requires an unattended simulated AI run.' }
    if($PSBoundParameters.ContainsKey('AfterPairs') -and $AfterPairs -ne 1) { throw 'Sign-transition capture arms after the first pair; omit AfterPairs or use 1.' }
    $AfterPairs=1
}
if($RecordedViewsFile -and ($Mode -ne 'Simulated' -or $Preflight)) { throw 'Recorded overrides require a simulated run.' }
if($CycleCamera -and ($Mode -ne 'Simulated' -or $PlaybackMode -ne 'benchmark')) { throw 'Camera cycling requires a simulated AI benchmark.' }
$root=Split-Path $PSScriptRoot -Parent
$cache=Get-Content -LiteralPath (Join-Path $root 'build/win32/CMakeCache.txt') -Raw
if($cache -notmatch '(?m)^_?Python3_EXECUTABLE:(?:FILEPATH|INTERNAL)=(.+)\r?$') { throw 'Configured Python is missing.' }
$python=$Matches[1].Trim()
$selection=Join-Path $root ('artifacts/automated-flicker-selection-'+[guid]::NewGuid().ToString('N'))
$selectionArgs=@('prepare','--output',$selection,'--car',$Car,'--track',$Track,'--mode',$PlaybackMode,'--control','ai','--laps',"$Laps")
if($CycleCamera) { $selectionArgs+='--cycle-camera' }
& $python (Join-Path $PSScriptRoot 'session_config.py') @selectionArgs
if($LASTEXITCODE) { throw 'Automated selected session preparation failed.' }
$parameters=@{
    Mode=$(if($Preflight){'Preflight'}elseif($Mode -eq 'Simulated'){'Desktop'}else{'Headset'})
    XrRuntime=$XrRuntime;HeadsetScale=$HeadsetScale;Frames=900;CaptureAfterSeconds=$CaptureAfterSeconds;TimeoutSeconds=$TimeoutSeconds
    RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;FlickerCaptureAfterPairs=$AfterPairs
    FlickerObservationSeconds=$ObservationSeconds
    FlickerSignTarget=[bool]$CaptureSignTransition
    NativeVisibilityControl=[bool]$NativeVisibilityControl
    TrackedVisibility=[bool]$TrackedVisibility
    MirrorPreview=[bool]$MirrorPreview
    VisibilityCapture=[bool]$VisibilityCapture
    ScreenUiStereo=[bool]$ScreenUiStereo
    InstanceCapacity=$InstanceCapacity
    DiagnosticProxy=$DiagnosticProxy;BackgroundPlay='Suppress';SessionDirectory=$selection;PythonPath=$python
    HardwareSettingsFile=(Join-Path $PSScriptRoot 'practice-settings.xml')
}
if($RecordedViewsFile) { $parameters.RecordedViewsFile=$RecordedViewsFile }
Write-Output "Automatic flicker diagnostic: $Mode; native AI driving; eight-scene capture plus 180-scene lightweight observation, then owned-process recovery."
if($CaptureSignTransition) { Write-Output 'Capture is requested after the first stereo pair; the renderer waits for the verified sign placement before starting the eight-scene burst.' }
if($NativeVisibilityControl) { Write-Output 'Diagnostic control: retain the native visibility volume while replaying both eyes. This is not a launcher setting or a headset visibility test.' }
if($TrackedVisibility) { Write-Output 'Diagnostic candidate: fit a perspective visibility volume to the current eye pair; other VR settings remain unchanged.' }
if($VisibilityCapture) { Write-Output 'Reduced capture: visibility geometry, draw totals and eight completed image triples; no per-draw payloads. The bounded 180-scene watch remains enabled.' }
if($Mode -eq 'Simulated') { Write-Output 'Recorded poses exercise scene stereo. OpenXR and headset texture scaling are not exercised in this mode.' }
& (Join-Path $PSScriptRoot 'Run-StereoProbe.ps1') @parameters

param(
    [ValidateSet('Run','Restore')][string]$Action = 'Run',
    [string]$GameRoot = '',
    [string]$JournalPath = '',
    [ValidateSet('Active','Inactive')][string]$ProxyMode = 'Active',
    [ValidateSet('Probe','Benchmark')][string]$RunKind = 'Probe',
    [switch]$EngineTrace,
    [switch]$CaptureStageTargets,
    [switch]$TraceSceneBoundary,
    [switch]$TraceVisibility,
    [switch]$ExpandVisibility,
    [switch]$ReplayOnce,
    [switch]$TraceReplayWork,
    [switch]$TraceReplaySamples,
    [switch]$VerifyFreshReplayColor,
    [switch]$WaitForClipTransition,
    [switch]$ReplayEyeOffset,
    [ValidateRange(1,900)][int]$ReplayEyeFrames=1,
    [ValidateSet(-90,-60,-15,0,15,60,90)][int]$ReplayEyeYawDegrees=0,
    [switch]$ReplayAsymmetricFov,
    [switch]$ReplayFullPose,
    [switch]$ReplayStereo,
    [switch]$CompactStereoTrace,
    [string]$RecordedViewsFile='',
    [switch]$XrSessionProbe,
    [switch]$XrProjection,
    [ValidateSet(25,50,75,100,125,150,175,200)][int]$HeadsetScale=100,
    [switch]$DisableMotionBlur,
    [switch]$ScreenUiStereo,
    [switch]$FrontendSceneAudit,
    [switch]$WaitForStereoStart,
    [switch]$RuntimeSession,
    [switch]$SkipIntro,
    [switch]$RemoveCinemaMatte,
    [switch]$DiagnosticLogging,
    [string]$DrivingControls='',
    [ValidateSet('Off','HeadOnly','HideDriver')][string]$CockpitS2000='Off',
    [ValidateSet('Off','HeadOnly','HideDriver')][string]$CockpitMode='Off',
    [switch]$NativeCockpitExposure,
    [string]$CareerProfile='current',
    [ValidateRange(1.0,20.0)][ValidateScript({ $_ * 2 -eq [decimal]::Truncate($_ * 2) })][decimal]$HudDistance=1,
    [ValidateSet(0,1)][int]$HudFollowView=0,
    [ValidateSet(0,1)][int]$VrReplayCameras=1,
    [ValidateSet(0,1)][int]$VrGarageMenus=1,
    [ValidateRange(0,31)][int]$HudHiddenElements=1,
    [switch]$PerformanceDisplay,
    [ValidateSet(0,1)][int]$PerformanceFollowView=1,
    [switch]$MsaaAudit,
    [switch]$ExposureAudit,
    [int]$SceneWidth=0,
    [int]$SceneHeight=0,
    [switch]$FlickerDiagnostic,
    [switch]$AutoCaptureFlicker,
    [switch]$FlickerSignTarget,
    [switch]$NativeVisibilityControl,
    [switch]$TrackedVisibility,
    [switch]$HeadsetVisibilityCandidate,
    [switch]$MirrorPreview,
    [switch]$VisibilityCapture,
    [ValidateSet('Off','Observe','Expand')][string]$InstanceCapacity='Off',
    [ValidateRange(1,600)][int]$FlickerCaptureAfterPairs=120,
    [ValidateRange(0,180)][int]$FlickerObservationSeconds=0,
    [string]$DiagnosticProxy='',
    [int]$RecenterKey=121,
    [int]$RecenterModifiers=0,
    [switch]$RecenterTilt,
    [string]$RecenterDeviceBase64="",
    [switch]$UntilExit,
    [ValidateSet('Off','Observe','Human')][string]$PlayerControlProbe='Off',
    [switch]$PlayerControlOnly,
    [ValidateSet('Off','Observe','Suppress')][string]$BackgroundPlay='Off',
    [switch]$ProfileReplay,
    [Alias('SteamVrRuntime')][string]$XrRuntimeManifest='C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json',
    [switch]$RepeatAoDraw,
    [switch]$RepeatShadowDraw,
    [switch]$NormalizeShadowBindings,
    [switch]$IsolateReplayModels,
    [switch]$RestoreReplayComputeState,
    [switch]$SortReplayVpl,
    [switch]$IsolateReplayShadowSampler,
    [switch]$KeepCaptures,
    [switch]$DeferCaptureFinalization,
    [string]$PythonPath='',
    [string]$SessionDirectory='',
    [switch]$NormalLaunch,
    [ValidateRange(1,300)][int]$CaptureAfterSeconds = 30,
    [ValidateRange(30,600)][int]$TimeoutSeconds = 90
)
$ErrorActionPreference='Stop'
if($Action -eq 'Run' -and $NormalLaunch -and ($SessionDirectory -or !$RuntimeSession -or $PlayerControlProbe -ne 'Off' -or $RunKind -ne 'Probe')) { throw 'Normal launch requires the guarded runtime without a selected session or input probe.' }
if($MsaaAudit -and (!$RuntimeSession -or !$FlickerDiagnostic -or !$DiagnosticProxy -or $UntilExit -or $XrSessionProbe)) { throw 'MSAA audit requires an isolated bounded desktop diagnostic.' }
if($Action -eq 'Run' -and $ExposureAudit -and (!$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or !$DiagnosticProxy -or $UntilExit -or $XrSessionProbe -or $XrProjection -or $NormalLaunch)) { throw 'Exposure audit requires an isolated bounded unattended desktop diagnostic.' }
if($Action -eq 'Run' -and ($SceneWidth -or $SceneHeight) -and (!$RuntimeSession -or $PlayerControlOnly -or $SceneWidth -lt 640 -or $SceneWidth -gt 3840 -or $SceneHeight -lt 360 -or $SceneHeight -gt 2160)) { throw 'Invalid native VR scene-size request.' }
if($Action -eq 'Run' -and $ScreenUiStereo -and (!$RuntimeSession -or !$ReplayStereo -or !$ReplayFullPose -or !$ReplayAsymmetricFov -or !$TraceReplayWork)) { throw 'UI stereo candidate requires a fully posed stereo runtime and replay instrumentation.' }
if($Action -eq 'Run' -and $FlickerObservationSeconds -and (!$TrackedVisibility -or !$VisibilityCapture -or !$AutoCaptureFlicker -or !$RuntimeSession -or !$RecordedViewsFile -or $XrSessionProbe -or $XrProjection -or $UntilExit -or $HeadsetVisibilityCandidate -or $CaptureAfterSeconds+$FlickerObservationSeconds+20 -ge $TimeoutSeconds)) { throw 'Extended observation requires bounded simulated tracked visibility with reduced capture and enough timeout.' }
if($Action -eq 'Run' -and $HeadsetVisibilityCandidate -and (!$RuntimeSession -or !$UntilExit -or !$FlickerDiagnostic -or !$TrackedVisibility -or !$MirrorPreview -or !$VisibilityCapture -or $InstanceCapacity -ne 'Expand' -or !$DiagnosticProxy -or !$XrSessionProbe -or !$XrProjection -or !$ReplayStereo -or !$ReplayFullPose -or !$ReplayAsymmetricFov -or !$ExpandVisibility -or $AutoCaptureFlicker -or $RecordedViewsFile -or $NativeVisibilityControl -or $FlickerSignTarget -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off' -or $WaitForStereoStart -or $RunKind -ne 'Probe')) { throw 'Headset visibility candidate requires isolated live-XR human practice with qualified capacity and no recorded/automatic capture.' }
if($Action -eq 'Run' -and $InstanceCapacity -ne 'Off' -and (!$TrackedVisibility -or !$VisibilityCapture)) { throw 'Instance-capacity diagnostics require tracked visibility and reduced capture.' }
if($Action -eq 'Run' -and $VisibilityCapture -and ((!$TrackedVisibility -and !$NativeVisibilityControl) -or $FlickerSignTarget)) { throw 'Visibility-only capture requires a tracked or native visibility diagnostic without sign-target capture.' }
if($Action -eq 'Run' -and $MirrorPreview -and !$TrackedVisibility) { throw 'Mirror preview requires the isolated tracked-visibility diagnostic.' }
if($Action -eq 'Run' -and $TrackedVisibility -and !$HeadsetVisibilityCandidate -and ($NativeVisibilityControl -or !$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or !$ReplayStereo -or !$RecordedViewsFile -or !$ReplayFullPose -or !$ReplayAsymmetricFov -or !$ExpandVisibility -or $XrSessionProbe -or $XrProjection -or $UntilExit -or $WaitForStereoStart -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off' -or $RunKind -ne 'Probe')) { throw 'Tracked visibility requires exclusive bounded unattended simulated AI diagnostics.' }
if($Action -eq 'Run' -and $NativeVisibilityControl -and (!$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or !$ReplayStereo -or !$RecordedViewsFile -or $XrSessionProbe -or $XrProjection -or $UntilExit -or $WaitForStereoStart -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off' -or $RunKind -ne 'Probe')) { throw 'Native-visibility control requires bounded unattended simulated AI diagnostics.' }
if($Action -eq 'Run' -and $FlickerSignTarget) {
    if(!$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or !$ReplayStereo -or !$RecordedViewsFile -or $XrSessionProbe -or $XrProjection -or $UntilExit -or $WaitForStereoStart -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off' -or $RunKind -ne 'Probe') { throw 'Sign-target capture requires unattended simulated AI flicker diagnostics.' }
    if($PSBoundParameters.ContainsKey('FlickerCaptureAfterPairs') -and $FlickerCaptureAfterPairs -ne 1) { throw 'Sign-target capture must be requested after the first stereo pair.' }
    $FlickerCaptureAfterPairs=1
}
if($Action -eq 'Run' -and $RemoveCinemaMatte -and (!$RuntimeSession -or !$ReplayStereo -or $PlayerControlOnly -or
    (!$XrProjection -and (!$RecordedViewsFile -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or !$DiagnosticProxy -or $UntilExit)))) {
    throw 'Cinema matte removal requires VR or an explicit bounded simulated-eye diagnostic.'
}
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'Recenter-Key.ps1')
Assert-RecenterKey $RecenterKey $RecenterModifiers
. (Join-Path $PSScriptRoot 'Recenter-Device.ps1')
Assert-RecenterDevice (Split-Path $PSScriptRoot -Parent) $RecenterDeviceBase64
. (Join-Path $PSScriptRoot 'Practice-Flow.ps1')
. (Join-Path $PSScriptRoot 'Native-Startup.ps1')
. (Join-Path $PSScriptRoot 'Cinema-Matte.ps1')
. (Join-Path $PSScriptRoot 'Cockpit-Camera.ps1')
. (Join-Path $PSScriptRoot 'Cockpit-Assets.ps1')
. (Join-Path $PSScriptRoot 'Instance-Capacity.ps1')
. (Join-Path $PSScriptRoot 'OpenXR-Runtime.ps1')
. (Join-Path $PSScriptRoot 'Flicker-Automation.ps1')
if(!$GameRoot) { $GameRoot=Get-Grid2GameRoot $root }
$game=[IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
$journalPath=if($JournalPath){[IO.Path]::GetFullPath($JournalPath)}else{Join-Path (Get-Grid2DataRoot $root) 'diagnostic-pending.json'}
$allowed=@('d3d11.dll','GRID2VR.diagnostic-session','GRID2VR.session.xml')
function Restore-Diagnostic {
    if (!(Test-Path -LiteralPath $journalPath)) { return }
    if (Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'The game must exit before removing the diagnostic proxy.' }
    $journal=Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json
    if ($journal.gameRoot -ne $game) { throw 'Recovery journal belongs to a different installation.' }
    foreach ($entry in $journal.files) {
        if ($entry.name -notin $allowed) { throw 'Unexpected journal target.' }
        $target=[IO.Path]::GetFullPath((Join-Path $game $entry.name))
        if ([IO.Path]::GetDirectoryName($target) -ne $game) { throw 'Recovery target escapes the game root.' }
        if (Test-Path -LiteralPath $target) {
            if (((Get-Item -LiteralPath $target -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Recovery target is linked.' }
            if ((Get-FileHash -LiteralPath $target).Hash -ne $entry.sha256) { throw "Changed diagnostic file retained: $target" }
        }
    }
    if($journal.cockpitCamera) { Restore-CockpitCamera $game $journal.run $journal.cockpitCamera }
    if($journal.cockpitAssets) { Restore-CockpitAssets $game $journal.run $journal.cockpitAssets }
    if($journal.cinemaMatte) { Restore-CinemaMatte $game $journal.run $journal.cinemaMatte }
    if($journal.instanceCapacity) { Restore-InstanceCapacity $game $journal.run $journal.instanceCapacity }
    if($journal.startupMovies) { Restore-StartupMovies $game $journal.run $journal.startupMovies }
    if($journal.practiceFlow) { Restore-PracticeFlow $game $journal.run $journal.practiceFlow }
    foreach($entry in $journal.files) {
        $target=Join-Path $game $entry.name
        if(Test-Path -LiteralPath $target) {
            if((Get-FileHash -LiteralPath $target).Hash -ne $entry.sha256) { throw "Changed diagnostic file retained: $target" }
            Remove-Item -LiteralPath $target
        }
    }
    Move-Item -LiteralPath $journalPath -Destination (Join-Path $journal.run 'deployment-restored.json')
    Write-Output 'Diagnostic files removed; any owned practice flow restored and hash checked.'
}
if ($Action -eq 'Restore') { Restore-Diagnostic; return }
. (Join-Path $PSScriptRoot 'Driving-Controls.ps1')
$validatedDrivingControls=Read-DrivingControls $DrivingControls
if (Test-Path -LiteralPath (Join-Path (Get-Grid2DataRoot $root) 'practice-flow-pending.json')) { throw 'Restore the earlier experimental practice flow before launching.' }
$selectedSession=$null
if ($SessionDirectory) {
    if (!$PythonPath) {
        $PythonPath=Get-Grid2Python $root
    }
    # Revalidate before creating a run, backup, journal or deployment file.
    $verified=& $PythonPath (Join-Path $PSScriptRoot 'session_config.py') --game-root $game verify --session $SessionDirectory
    if ($LASTEXITCODE) { throw 'Selected session validation failed; nothing deployed.' }
    $selectedSession=$verified | ConvertFrom-Json
}
if($CockpitS2000 -ne 'Off') {
    if(!$RuntimeSession -or !$UntilExit -or $PlayerControlProbe -ne 'Off' -or (!$NormalLaunch -and $selectedSession.selection.car -cne 's20')) { throw 'Cockpit prototype requires a user-run S2000 session or Normal mode.' }
    Assert-CockpitCameraOriginal $game
    $null=Get-CockpitCameraCandidate $root $CockpitS2000
}
$cockpitAssets=@()
if($CockpitMode -ne 'Off') {
    if(!$RuntimeSession -or (!$ExposureAudit -and (!$UntilExit -or !$XrProjection)) -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off' -or $CockpitS2000 -ne 'Off') { throw 'Launcher cockpit assets require a VR user session or bounded exposure audit.' }
    $cockpitAssets=@(Get-CockpitAssetSelection $root $game $CockpitMode $selectedSession.selection.car ([bool]$NormalLaunch))
}
if ($PlayerControlProbe -ne 'Off' -and (!$RuntimeSession -or !$selectedSession -or $selectedSession.selection.mode -ne 'attract' -or (!$RecordedViewsFile -and !$PlayerControlOnly) -or $XrSessionProbe -or $XrProjection)) { throw 'Player input probes require a selected solo attract session and the desktop runtime.' }
if ($selectedSession -and $selectedSession.control -eq 'human' -and (!$RuntimeSession -or $PlayerControlProbe -ne 'Off')) { throw 'Human practice uses the guarded runtime and native controls, with the old input probe off.' }
if ($PlayerControlOnly -and (($PlayerControlProbe -eq 'Off' -and $BackgroundPlay -eq 'Off') -or $EngineTrace -or $CaptureStageTargets -or $ReplayOnce -or $RecordedViewsFile -or $ProxyMode -ne 'Active' -or $RunKind -ne 'Probe')) { throw 'Non-rendering probes require player control or background-play observations and cannot request engine rendering hooks or captures.' }
if ($BackgroundPlay -ne 'Off' -and (!$RuntimeSession -or $ProxyMode -ne 'Active')) { throw 'Background play requires an active guarded runtime session.' }
if ($UntilExit -and (!$RuntimeSession -or (!$NormalLaunch -and (!$selectedSession -or $selectedSession.control -ne 'human')) -or $RunKind -ne 'Probe')) { throw 'User-run sessions require guarded human practice/race or explicit Normal launch.' }
if ($RunKind -eq 'Benchmark' -and !$PSBoundParameters.ContainsKey('TimeoutSeconds')) { $TimeoutSeconds=240 }
if ($CaptureAfterSeconds -ge $TimeoutSeconds) { throw 'CaptureAfterSeconds must be less than TimeoutSeconds.' }
if ($RunKind -eq 'Probe' -and $ProxyMode -ne 'Active') { throw 'Short probes require an active proxy. Use -RunKind Benchmark for an inactive control.' }
if ($CaptureStageTargets -and !$EngineTrace) { throw 'CaptureStageTargets requires EngineTrace.' }
if ($TraceSceneBoundary -and !$EngineTrace) { throw 'TraceSceneBoundary requires EngineTrace.' }
if ($TraceVisibility -and !$EngineTrace) { throw 'TraceVisibility requires EngineTrace.' }
if ($ReplayOnce -and (!$TraceSceneBoundary -or !$EngineTrace -or !$CaptureStageTargets -or $RunKind -ne 'Probe' -or $ProxyMode -ne 'Active')) { throw 'ReplayOnce requires an active Probe with EngineTrace, TraceSceneBoundary and CaptureStageTargets.' }
if ($WaitForStereoStart -and (!$ReplayStereo -or !$ReplayOnce)) { throw 'WaitForStereoStart requires a guarded stereo probe.' }
if ($TraceReplayWork -and !$ReplayOnce) { throw 'TraceReplayWork requires ReplayOnce.' }
if ($TraceReplaySamples -and !$TraceReplayWork) { throw 'TraceReplaySamples requires TraceReplayWork.' }
if ($VerifyFreshReplayColor -and !$TraceReplayWork) { throw 'VerifyFreshReplayColor requires TraceReplayWork.' }
if ($WaitForClipTransition -and !$ReplayOnce) { throw 'WaitForClipTransition requires ReplayOnce.' }
if ($ReplayEyeOffset -and (!$TraceReplayWork -or ($WaitForClipTransition -and !$ReplayStereo))) { throw 'ReplayEyeOffset requires TraceReplayWork; targeted clip transitions require ReplayStereo.' }
if ($ReplayEyeFrames -gt 1 -and (!$ReplayEyeOffset -or !$RestoreReplayComputeState -or !$SortReplayVpl -or !$IsolateReplayShadowSampler -or !$NormalizeShadowBindings -or $IsolateReplayModels -or $RepeatAoDraw -or $RepeatShadowDraw -or $TraceReplaySamples)) { throw 'Eye sequences require ReplayEyeOffset, RestoreReplayComputeState, SortReplayVpl, IsolateReplayShadowSampler and NormalizeShadowBindings; model substitution and producer-repeat/MSAA experiments cannot be combined.' }
if ($ReplayEyeYawDegrees -ne 0 -and $ReplayEyeFrames -lt 2) { throw 'Rotated views require a bounded eye sequence with owned previous camera history.' }
if ([math]::Abs($ReplayEyeYawDegrees) -gt 15 -and !$ExpandVisibility) { throw 'Wide yaw probes require ExpandVisibility.' }
if ($ReplayAsymmetricFov -and $ReplayEyeFrames -lt 2) { throw 'Asymmetric FOV requires a bounded eye sequence with owned previous camera history.' }
if ($ReplayFullPose -and ($ReplayEyeFrames -lt 2 -or !$ExpandVisibility -or $ReplayEyeYawDegrees -ne 0)) { throw 'Full-pose probes require a bounded sequence, ExpandVisibility and no separate yaw request.' }
if ($ReplayStereo -and (!$ReplayFullPose -or !$ReplayAsymmetricFov)) { throw 'Stereo probes require ReplayFullPose and ReplayAsymmetricFov.' }
if ($CompactStereoTrace -and (!$ReplayStereo -or (!$VerifyFreshReplayColor -and !$RuntimeSession) -or $ReplayEyeFrames -lt 2)) { throw 'CompactStereoTrace requires a stereo sequence and VerifyFreshReplayColor for capture diagnostics.' }
if ($RuntimeSession -and !$PlayerControlOnly -and (!$ReplayStereo -or !$CompactStereoTrace -or !$DisableMotionBlur -or (!$RecordedViewsFile -and !$XrProjection) -or $WaitForStereoStart -or $KeepCaptures -or $ProfileReplay -or $WaitForClipTransition)) { throw 'RuntimeSession requires the full stereo path with recorded views or XR projection, motion blur disabled, and no capture/manual-gate experiments.' }
if ($ReplayEyeFrames -gt 120 -and !$CompactStereoTrace) { throw 'More than 120 frames requires CompactStereoTrace with bounded packed CPU evidence.' }
if ($RecordedViewsFile -and (!$CompactStereoTrace -or $XrSessionProbe -or $XrProjection -or !(Test-Path -LiteralPath $RecordedViewsFile -PathType Leaf))) { throw 'Recorded views require CompactStereoTrace, an existing input file and no OpenXR session.' }
if ($XrProjection -and (!$XrSessionProbe -or !$CompactStereoTrace -or !$DisableMotionBlur)) { throw 'XrProjection requires XrSessionProbe, CompactStereoTrace and DisableMotionBlur.' }
if ($DisableMotionBlur -and (!$ReplayStereo -or $ReplayEyeFrames -lt 2)) { throw 'DisableMotionBlur requires a bounded stereo sequence.' }
if ($ProfileReplay -and !$CompactStereoTrace) { throw 'ProfileReplay requires CompactStereoTrace.' }
if ($XrSessionProbe -and !$ReplayStereo) { throw 'OpenXR session probes require ReplayStereo.' }
if ($XrSessionProbe) { $runtimeInfo=Resolve-Grid2VrRuntime $XrRuntimeManifest }
if ($ExpandVisibility -and (!$TraceVisibility -or $ReplayEyeFrames -lt 2)) { throw 'ExpandVisibility requires TraceVisibility and a bounded eye sequence.' }
if ($RepeatAoDraw -and !$TraceReplayWork) { throw 'RepeatAoDraw requires TraceReplayWork.' }
if ($RepeatShadowDraw -and !$TraceReplayWork) { throw 'RepeatShadowDraw requires TraceReplayWork.' }
if ($NormalizeShadowBindings -and !$TraceReplayWork) { throw 'NormalizeShadowBindings requires TraceReplayWork.' }
if ($IsolateReplayModels -and !$TraceReplayWork) { throw 'IsolateReplayModels requires TraceReplayWork.' }
if ($RestoreReplayComputeState -and !$TraceReplayWork) { throw 'RestoreReplayComputeState requires TraceReplayWork.' }
if ($SortReplayVpl -and !$RestoreReplayComputeState) { throw 'SortReplayVpl requires RestoreReplayComputeState.' }
if ($IsolateReplayShadowSampler -and !$TraceReplayWork) { throw 'IsolateReplayShadowSampler requires TraceReplayWork.' }
if (Test-Path -LiteralPath $journalPath) { throw 'A recovery journal exists. Run -Action Restore first.' }
if (Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close the existing GRID 2 session first.' }
if (((Get-Item -LiteralPath $game).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Linked game root is not supported.' }
$expected='519CD994D2FF92A2A7A2A784856854183F53E359B83F8B9563CC09A0AE91E502'
if ((Get-FileHash -LiteralPath (Join-Path $game 'grid2_avx.exe')).Hash -ne $expected) { throw 'Unsupported AVX executable.' }
foreach ($name in $allowed + 'dxgi.dll') { if (Test-Path -LiteralPath (Join-Path $game $name)) { throw "Existing $name will not be overwritten." } }
$proxy=Get-Grid2Proxy $root
if($DiagnosticProxy) {
    if(!$FlickerDiagnostic) { throw 'A diagnostic proxy override requires FlickerDiagnostic.' }
    $proxy=(Resolve-Path -LiteralPath $DiagnosticProxy).Path
}
if($FlickerDiagnostic -and (!$RuntimeSession -or !$ReplayStereo)) { throw 'Flicker capture requires the stereo runtime.' }
if($AutoCaptureFlicker -and (!$FlickerDiagnostic -or $UntilExit -or !$selectedSession -or $selectedSession.control -ne 'ai' -or $selectedSession.selection.mode -notin @('attract','benchmark') -or $RunKind -ne 'Probe')) { throw 'Automatic flicker capture requires a bounded stereo diagnostic with selected AI playback.' }
if (!(Test-Path -LiteralPath $proxy)) { throw 'Build the proxy first.' }
. (Join-Path $PSScriptRoot 'User-Session.ps1')
. (Join-Path $PSScriptRoot 'Profiles.ps1')
$career=Get-LaunchProfile -Root $root -Id $CareerProfile
if($career.id -ne 'current') {
    if($ProxyMode -ne 'Active') { throw 'Managed careers require the active save provider.' }
    $proxy=Get-CareerProfileProxy -Root $root -BaseProxy $proxy
    $binary=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($proxy))
    if(!$binary.Contains('GRID2VR_PROFILE_STORAGE_V1')) { throw 'This native build does not support separate careers. Rebuild before launching.' }
}
$run=Join-Path (Get-Grid2DataRoot $root) ('diagnostic-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$drivingControlsReceipt=Save-DrivingControls $validatedDrivingControls $run
$steamArgumentLine=if($NormalLaunch){'-applaunch 44350'}else{'-applaunch 44350 -benchmark example_benchmark.xml'}
if ($selectedSession) {
    # Freeze a private XML copy for this run. The backend rejects changed inputs,
    # linked paths and existing output. GRID 2 receives an owned relative filename.
    $staged=& $PythonPath (Join-Path $PSScriptRoot 'session_config.py') --game-root $game stage --session $SessionDirectory --run-directory $run
    if ($LASTEXITCODE) { throw 'Selected session staging failed; nothing deployed.' }
    $selectedSession=$staged | ConvertFrom-Json
    $steamArgumentLine=$selectedSession.steamArgumentLine
    $selectedSession | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'selected-session.json') -Encoding utf8
}
if ($RuntimeSession) { [IO.File]::WriteAllText((Join-Path $run 'runtime-session.request'),'Sustained same-frame stereo with explicit suspension and bounded logs; guarded experimental runtime.') }
if ($DiagnosticLogging -or !$UntilExit) { [IO.File]::WriteAllText((Join-Path $run 'diagnostic-logging.request'),'Enable additional bounded troubleshooting details; never disable recovery receipts.') }
if($NormalLaunch) { [IO.File]::WriteAllText((Join-Path $run 'normal-launch.request'),'Native menus; no selected-session flow or configuration.') }
if($selectedSession -and $selectedSession.selection.mode -eq 'race') { [IO.File]::WriteAllText((Join-Path $run 'race-launch.request'),'Selected human race with AI opponents.') }
if($RuntimeSession -and $ReplayStereo -and !$PlayerControlOnly -and !$VrReplayCameras) { [IO.File]::WriteAllText((Join-Path $run 'flat-replays.request'),'Use the shared screen for native replay cameras.') }
if($RuntimeSession -and $ReplayStereo -and !$PlayerControlOnly -and !$VrGarageMenus) { [IO.File]::WriteAllText((Join-Path $run 'flat-garage.request'),'Use the shared screen for the native garage menus.') }
if($FrontendSceneAudit) { [IO.File]::WriteAllText((Join-Path $run 'frontend-scene-audit.request'),'Bounded passive frontend scene audit.') }
if($ExposureAudit) { [IO.File]::WriteAllText((Join-Path $run 'exposure-audit.request'),'At most 600 luminance/constant rows; no full-size images.') }
if($CockpitMode -ne 'Off' -and !$NativeCockpitExposure) { [IO.File]::WriteAllText((Join-Path $run 'cockpit-exposure.request'),'Retain the authored exposure ceiling in the exact cockpit camera only.') }
if ($RuntimeSession -and $ReplayStereo -and !$PlayerControlOnly) {
    $hudRequest=$HudDistance.ToString([Globalization.CultureInfo]::InvariantCulture)+" $HudFollowView $HudHiddenElements"
    [IO.File]::WriteAllText((Join-Path $run 'vr-hud.request'),$hudRequest,[Text.Encoding]::ASCII)
}
if ($PerformanceDisplay) {
    [IO.File]::WriteAllText((Join-Path $run 'performance-display.request'),'Show submitted app FPS and adapter-wide GPU usage in VR; unavailable readings remain N/A.')
    if (!$PerformanceFollowView) { [IO.File]::WriteAllText((Join-Path $run 'performance-display-fixed.request'),'Fixed level panel below the recentered forward view.') }
}
if ($MsaaAudit) { [IO.File]::WriteAllText((Join-Path $run 'msaa-audit.request'),'Bounded read-only D3D texture allocation audit; no native parser hook.') }
if ($SceneWidth) { [IO.File]::WriteAllText((Join-Path $run 'scene-size.request'),"$SceneWidth $SceneHeight",[Text.Encoding]::ASCII) }
if ($FlickerDiagnostic) { [IO.File]::WriteAllText((Join-Path $run 'flicker-diagnostic.request'),'Opt-in, one bounded passive capture burst; no graphics settings changes.') }
if ($AutoCaptureFlicker -or $HeadsetVisibilityCandidate) { [IO.File]::WriteAllText((Join-Path $run 'flicker-compact.request'),'Keep object fields, PS CB5, streams and pipeline state; omit redundant whole-buffer stamps.') }
if ($AutoCaptureFlicker) { [IO.File]::WriteAllText((Join-Path $run 'flicker-watch.request'),'After the eight-scene burst, observe 180 moving scenes with bounded nonblocking draw queries.') }
if ($FlickerSignTarget) { [IO.File]::WriteAllText((Join-Path $run 'flicker-sign-target.request'),'After the automatic capture request, wait for the verified CPU sign placement before arming the eight-scene burst.') }
if ($NativeVisibilityControl) { [IO.File]::WriteAllText((Join-Path $run 'native-visibility-control.request'),'Diagnostic comparison only: retain the native visibility volume while preserving same-frame eye replay and restoration guards. Bounded unattended simulated AI capture only; not a launcher setting.') }
if ($TrackedVisibility) { [IO.File]::WriteAllText((Join-Path $run 'tracked-visibility.request'),'Isolated candidate: enclose the current frozen eye frusta before preparation. Original native corners are not included; not a launcher default.') }
if ($HeadsetVisibilityCandidate) { [IO.File]::WriteAllText((Join-Path $run 'headset-visibility-candidate.request'),'Isolated live OpenXR candidate: frozen tracked eyes, qualified native capacities, existing recenter/suspension. No automatic raw capture.') }
if ($MirrorPreview) { [IO.File]::WriteAllText((Join-Path $run 'mirror-preview.request'),'Diagnostic only: mirror this frame completed left eye onto a matching native backbuffer. No stale frame, scaling or colour conversion.') }
if ($VisibilityCapture) { [IO.File]::WriteAllText((Join-Path $run 'flicker-visibility-only.request'),'Diagnostic only: count draws and retain visibility geometry and complete image triples; omit per-draw payloads.') }
if ($InstanceCapacity -ne 'Off') { [IO.File]::WriteAllText((Join-Path $run 'instance-capacity-observe.request'),'Read-only D3D buffer creation and native frame demand; no early native code hooks.') }
if ($InstanceCapacity -eq 'Expand') { [IO.File]::WriteAllText((Join-Path $run 'instance-capacity-expanded.request'),'Require the native high-profile capacity candidate before stereo.') }
if ($AutoCaptureFlicker -and $RecordedViewsFile -and !$XrProjection -and !$XrSessionProbe) {
    [IO.File]::WriteAllText((Join-Path $run 'flicker-unattended.request'),'Selected AI playback with recorded views: allow visible, unminimized background rendering without input automation.')
}
if ($RuntimeSession -and $XrProjection) { [IO.File]::WriteAllText((Join-Path $run 'runtime-recenter-key.txt'),"$RecenterKey $RecenterModifiers") }
if ($RuntimeSession -and $XrProjection -and $RecenterTilt) { [IO.File]::WriteAllText((Join-Path $run 'runtime-recenter-tilt.request'),'') }
if ($PlayerControlProbe -ne 'Off') { [IO.File]::WriteAllText((Join-Path $run 'player-control.request'),$PlayerControlProbe.ToLowerInvariant()) }
if ($BackgroundPlay -ne 'Off') { [IO.File]::WriteAllText((Join-Path $run 'background-play.request'),$BackgroundPlay.ToLowerInvariant()) }
if ($RunKind -eq 'Probe' -and !$RuntimeSession) {
    [IO.File]::WriteAllText((Join-Path $run 'short-probe.request'),'Signal after the first bounded capture window; this does not establish diagnostic success.')
}
if ($EngineTrace) {
    if ($ProxyMode -ne 'Active') { throw 'EngineTrace requires the active diagnostic proxy.' }
    [IO.File]::WriteAllText((Join-Path $run 'engine-trace.request'),'Observational engine trace; forward each call once.')
}
if ($CaptureStageTargets) {
    [IO.File]::WriteAllText((Join-Path $run 'stage-target-capture.request'),'Experimental passive render-target readback; requires runtime validation.')
}
if ($TraceSceneBoundary) {
    [IO.File]::WriteAllText((Join-Path $run 'scene-boundary-trace.request'),'Observe complete-scene arguments and return.')
}
if ($TraceVisibility) {
    [IO.File]::WriteAllText((Join-Path $run 'visibility-trace.request'),'Observe native visibility volume, camera and consumer.')
}
if ($ExpandVisibility) { [IO.File]::WriteAllText((Join-Path $run 'expand-visibility.request'),'Guarded bounded all-direction visibility-volume experiment; restore after each scene pair.') }
if ($ReplayOnce) { [IO.File]::WriteAllText((Join-Path $run 'replay-once.request'),'One guarded identical-view complete-scene replay; experimental, not stereo.') }
if ($ReplayEyeOffset) { [IO.File]::WriteAllText((Join-Path $run 'eye-offset.request'),'One translated geometry view at +0.064 native world units, within the same frame. Not calibrated stereo or headset output.') }
if ($ReplayEyeFrames -gt 1) {
    [IO.File]::WriteAllText((Join-Path $run 'eye-sequence.request'),[string]$ReplayEyeFrames)
    [IO.File]::WriteAllText((Join-Path $run 'eye-ao-audit.request'),'Exact GPU handoff hashes for the first and last two pairs, plus final color ownership hashes. No additional raw capture files.')
    [IO.File]::WriteAllText((Join-Path $run 'eye-camera-history.request'),'Use the exact preceding rendered eye VP after the first bootstrap pair.')
}
if ($ReplayEyeYawDegrees -ne 0) { [IO.File]::WriteAllText((Join-Path $run 'eye-yaw.request'),[string]$ReplayEyeYawDegrees) }
if ($ReplayAsymmetricFov) { [IO.File]::WriteAllText((Join-Path $run 'eye-asymmetric-fov.request'),'Fixed diagnostic tangents: left=-1 right=.8 down=-.7 up=.6. Preserve native clip terms; not headset FOV or culling acceptance.') }
if ($ReplayFullPose) { [IO.File]::WriteAllText((Join-Path $run 'eye-pose.request'),'0.1 0.2 0.05 0.973396116 0.05 0.03 -0.02') }
if ($ReplayStereo) { [IO.File]::WriteAllText((Join-Path $run 'stereo-eyes.request'),'Center plus two same-frame posed geometry eyes, separation .064 native units; independent eye history and mirrored lateral FOV. Not calibrated headset output.') }
if ($CompactStereoTrace) { [IO.File]::WriteAllText((Join-Path $run 'compact-stereo.request'),'Keep first-scene GPU camera buffers and fresh-color seeds, first/last eye images, sampled AO hashes, and all CPU/state-restoration checks. Omit full mesh/texture/draw readbacks.') }
if ($CompactStereoTrace -and !$RuntimeSession) { [IO.File]::WriteAllText((Join-Path $run 'packed-snapshots.request'),'Pack unchanged bounded CPU snapshot bytes while rendering; unpack after recovery before independent analysis.') }
if ($ProfileReplay) { [IO.File]::WriteAllText((Join-Path $run 'profile-replay.request'),'CPU wall-time buckets, inclusive of nested scopes; driver timing includes CPU waits and is not GPU time.') }
if ($XrSessionProbe) { [IO.File]::WriteAllText((Join-Path $run 'xr-session.request'),$runtimeInfo.manifest,[Text.UTF8Encoding]::new($false)) }
if ($RecordedViewsFile) { [IO.File]::Copy((Get-Item -LiteralPath $RecordedViewsFile).FullName,(Join-Path $run 'recorded-views.request'),$false) }
if ($XrProjection) {
    . (Join-Path $PSScriptRoot 'Headset-Settings.ps1')
    Write-HeadsetRequest (Join-Path $run 'xr-projection.request') $HeadsetScale
}
if ($WaitForStereoStart) { [IO.File]::WriteAllText((Join-Path $run 'wait-stereo-start.request'),'Initialize and warm up first. Require a fresh stereo-start.request after manual unpaused-game confirmation before arming stereo. No input or focus automation.') }
if ($DisableMotionBlur) { [IO.File]::WriteAllText((Join-Path $run 'disable-motion-blur.request'),'Known final shaders only: zero the two velocity scale values in an owned CB0 copy; restore the original binding after each draw.') }
if ($ScreenUiStereo) { [IO.File]::WriteAllText((Join-Path $run 'screen-ui-stereo.request'),'Candidate: verified screen UI shaders and exact native UI cameras only. Align XY to a shared plane; restore original CB and clipping after each draw.') }
if ($WaitForClipTransition) { [IO.File]::WriteAllText((Join-Path $run 'wait-clip-transition.request'),'Arm after an observed native .1 or .2 / 15000 to .2 / 100 main-camera transition. Timeout remains unchanged; acceptance requires a changed captured frame and its following eye history.') }
if ($TraceReplayWork) { [IO.File]::WriteAllText((Join-Path $run 'replay-work.request'),'Bounded mapped-buffer trace, source-format captures and prepared-list snapshots for the first original/replay pair.') }
if ($TraceReplaySamples) { [IO.File]::WriteAllText((Join-Path $run 'replay-samples.request'),'Focus color captures on the first PS 922a49bc5dcd4775 draw and preserve exact RGB10A2 MSAA samples before and after it. Retain scene/producer checkpoints.') }
if ($VerifyFreshReplayColor) { [IO.File]::WriteAllText((Join-Path $run 'fresh-replay-color.request'),'Seed the guarded first single-sample main-color draw with magenta in the original pass and green in replay. Require exact seed readbacks and final-image equality to prove fresh output.') }
if ($VerifyFreshReplayColor) { [IO.File]::WriteAllText((Join-Path $run 'fresh-color-identity.request'),'The first native camera/sky material and target must match across center, left and right; never skip an earlier color draw.') }
if ($RepeatAoDraw) { [IO.File]::WriteAllText((Join-Path $run 'repeat-ao.request'),'Repeat the first exactly identified AO draw immediately from its saved incoming target; retain the native completed output and restore it afterward. Diagnostic only.') }
if ($RepeatShadowDraw) { [IO.File]::WriteAllText((Join-Path $run 'repeat-shadow.request'),'Repeat the exactly identified shadow-mask draw immediately from its saved incoming color target; refuse depth/stencil writes and restore the original color result afterward. Diagnostic only.') }
if ($NormalizeShadowBindings) { [IO.File]::WriteAllText((Join-Path $run 'normalize-shadow-bindings.request'),'Temporarily clear unused PS texture slots 5-127 and sampler slots 7-15 for the exact shadow-mask shader in both passes. Restore every binding after the native draw. Diagnostic only.') }
if ($IsolateReplayModels) { [IO.File]::WriteAllText((Join-Path $run 'isolate-models.request'),'Causal experiment: one identified shader uses the original model input for matching replay draws. Not a simulation-isolation or stereo implementation.') }
if ($RestoreReplayComputeState) { [IO.File]::WriteAllText((Join-Path $run 'restore-compute-state.request'),'Restore incoming compute counter/sampler bindings and guarded AO scratch contents for one replay; restore the completed bindings and scratch contents afterward.') }
if ($SortReplayVpl) { [IO.File]::WriteAllText((Join-Path $run 'sort-vpl.request'),'Sort active VPL indices independently after each native light-culling dispatch; preserve membership, terminator and unused tail.') }
if ($IsolateReplayShadowSampler) { [IO.File]::WriteAllText((Join-Path $run 'isolate-shadow-sampler.request'),'Use the original sampler 6 only for the matching guarded shadow-mask draw; restore the native replay binding immediately afterward.') }
if ($IsolateReplayShadowSampler) { [IO.File]::WriteAllText((Join-Path $run 'shadow-pass-identity.request'),'Match the unique shader-guarded shadow draw by target, depth, quad binding, layout, viewport, used resources and samplers 1-5; draw ordinal is recorded only.') }
# Retain the exact tested DLL so a later rebuild does not erase the control.
$proxySnapshot=Join-Path $run 'diagnostic-proxy.dll'
[IO.File]::Copy($proxy,$proxySnapshot,$false)
$proxyHash=(Get-FileHash -LiteralPath $proxySnapshot).Hash
if ((Get-FileHash -LiteralPath $proxy).Hash -ne $proxyHash) { throw 'Proxy changed while preparing deployment.' }
$marker=Join-Path $run 'GRID2VR.diagnostic-session'
Write-ProfileStorageRequest -Profile $career -Run $run
if ($ProxyMode -eq 'Active') { [IO.File]::WriteAllText($marker,"GRID2VR_DIAGNOSTIC_V1`n$run`n",[Text.UTF8Encoding]::new($false)) }
$entries=@(@{name='d3d11.dll';source=$proxySnapshot;sha256=$proxyHash})
if ($ProxyMode -eq 'Active') { $entries+=@{name='GRID2VR.diagnostic-session';source=$marker;sha256=(Get-FileHash -LiteralPath $marker).Hash} }
if ($selectedSession) {
    if ($selectedSession.nativeConfigurationName -cne 'GRID2VR.session.xml' -or
        (Get-FileHash -LiteralPath $selectedSession.configuration).Hash -ne $selectedSession.configurationSha256) { throw 'Selected configuration deployment differs.' }
    $entries+=@{name='GRID2VR.session.xml';source=$selectedSession.configuration;sha256=$selectedSession.configurationSha256}
}
# Persist ownership before copying, so recovery works after interrupted deployment.
$practiceFlow=$null
if($selectedSession -and $selectedSession.control -eq 'human') { $practiceFlow=New-PracticeFlowDeployment $game $run }
$startupMovies=$null # Retain the journal field for recovery of older deployments.
$nativeStartup=if($SkipIntro) { Write-NativeStartupRequest $game $run } else { $null }
$cinemaMatte=$null
if($RemoveCinemaMatte) { $cinemaMatte=New-CinemaMatteDeployment $game $run }
$cockpitCamera=$null
if($CockpitS2000 -ne 'Off') { $cockpitCamera=New-CockpitCameraDeployment $game $run $CockpitS2000 $root }
if($cockpitAssets.Count) {
    $cockpitAssets=@(New-CockpitAssetsDeployment $game $run $root $cockpitAssets)
}
$instanceDeployment=$null
if($InstanceCapacity -eq 'Expand') {
    $converter=Get-Grid2Converter $root
    & $PythonPath (Join-Path $PSScriptRoot 'instance_capacity_config.py') --game-root $game --output (Join-Path $run 'instance-capacity-prepared') --converter $converter
    if($LASTEXITCODE) { throw 'Native capacity configuration preparation failed; nothing deployed.' }
    $instanceDeployment=New-InstanceCapacityDeployment $game $run
}
@{gameRoot=$game;run=$run;files=$entries;practiceFlow=$practiceFlow;startupMovies=$startupMovies;cinemaMatte=$cinemaMatte;cockpitCamera=$cockpitCamera;cockpitAssets=$cockpitAssets;instanceCapacity=$instanceDeployment} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $journalPath
$receipt=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');runKind=$RunKind;timeoutSeconds=$TimeoutSeconds;captureAfterSeconds=$CaptureAfterSeconds;proxyMode=$ProxyMode;engineTrace=[bool]$EngineTrace;captureStageTargets=[bool]$CaptureStageTargets;traceSceneBoundary=[bool]$TraceSceneBoundary;replayOnce=[bool]$ReplayOnce;executableSha256=$expected;proxySha256=$entries[0].sha256;processes=@();terminatedOwnedProcessIds=@();result='prepared';uiAutomation=$false}
$receipt.vrHud=@{enabled=([bool]$RuntimeSession -and [bool]$ReplayStereo -and !$PlayerControlOnly);distance=$HudDistance;followView=[bool]$HudFollowView;hiddenElements=$HudHiddenElements}
$receipt.performanceDisplay=[bool]$PerformanceDisplay
$receipt.performanceFollowView=[bool]$PerformanceFollowView
$receipt.career=$career
$receipt.traceVisibility=[bool]$TraceVisibility
$receipt.expandVisibility=[bool]$ExpandVisibility
$receipt.runtimeSession=[bool]$RuntimeSession
$receipt.skipIntro=[bool]$SkipIntro
$receipt.nativeStartup=$nativeStartup
$receipt.cockpitCamera=$cockpitCamera
$receipt.cockpitAssets=$cockpitAssets
$receipt.cockpitMode=$CockpitMode
$receipt.cockpitExposureLimit=($CockpitMode -ne 'Off' -and !$NativeCockpitExposure)
$receipt.exposureAudit=[bool]$ExposureAudit
$receipt.removeCinemaMatte=[bool]$RemoveCinemaMatte
$receipt.diagnosticLogging=[bool]$DiagnosticLogging
$receipt.drivingControls=$drivingControlsReceipt
$receipt.msaaAudit=[bool]$MsaaAudit
$receipt.nativeSceneResolution=@{enabled=($SceneWidth -gt 0);width=$SceneWidth;height=$SceneHeight}
$receipt.flickerDiagnostic=[bool]$FlickerDiagnostic
$receipt.automaticFlicker=@{enabled=[bool]$AutoCaptureFlicker;afterPairs=$FlickerCaptureAfterPairs;requested=$false;outcome='not_requested';simulatedViews=[bool]$RecordedViewsFile;signTarget=[bool]$FlickerSignTarget;nativeVisibilityControl=[bool]$NativeVisibilityControl;trackedVisibility=[bool]$TrackedVisibility}
$receipt.automaticFlicker.mirrorPreview=[bool]$MirrorPreview
$receipt.automaticFlicker.visibilityCapture=[bool]$VisibilityCapture
$receipt.automaticFlicker.instanceCapacity=$InstanceCapacity
$receipt.automaticFlicker.observationSeconds=$FlickerObservationSeconds
if($FlickerObservationSeconds) { [IO.File]::WriteAllText((Join-Path $run 'flicker-observation.request'),"$FlickerObservationSeconds") }
$receipt.headsetVisibilityCandidate=[bool]$HeadsetVisibilityCandidate
$receipt.recenterKey=$RecenterKey
$receipt.recenterModifiers=$RecenterModifiers
$receipt.keepHorizonLevel=!$RecenterTilt
$receipt.untilExit=[bool]$UntilExit
$receipt.normalLaunch=[bool]$NormalLaunch
$receipt.vrReplayCameras=[bool]$VrReplayCameras
$receipt.vrGarageMenus=[bool]$VrGarageMenus
$receipt.frontendSceneAudit=[bool]$FrontendSceneAudit
$receipt.playerControlProbe=$PlayerControlProbe
$receipt.playerControlOnly=[bool]$PlayerControlOnly
$receipt.backgroundPlay=$BackgroundPlay
if ($selectedSession) { $receipt.selectedSession=$selectedSession }
$before=@(Get-ChildItem -LiteralPath (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\GRID 2') -Recurse -File | ForEach-Object {@{path=$_.FullName;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
$receipt.traceReplayWork=[bool]$TraceReplayWork
$receipt.replayEyeOffset=[bool]$ReplayEyeOffset
$receipt.replayEyeFrames=$ReplayEyeFrames
$receipt.replayEyeYawDegrees=$ReplayEyeYawDegrees
$receipt.replayAsymmetricFov=[bool]$ReplayAsymmetricFov
$receipt.replayFullPose=[bool]$ReplayFullPose
$receipt.replayStereo=[bool]$ReplayStereo
$receipt.compactStereoTrace=[bool]$CompactStereoTrace
$receipt.xrSessionProbe=[bool]$XrSessionProbe
$receipt.xrProjection=[bool]$XrProjection
if ($XrProjection) { $receipt.headsetSettings=Get-HeadsetSettings $HeadsetScale }
$receipt.disableMotionBlur=[bool]$DisableMotionBlur
$receipt.screenUiStereo=[bool]$ScreenUiStereo
if ($RecordedViewsFile) { $receipt.recordedViewsSha256=(Get-FileHash -LiteralPath (Join-Path $run 'recorded-views.request')).Hash }
if ($XrSessionProbe) { $receipt.xrRuntime=$runtimeInfo.name; $receipt.xrRuntimeManifest=$runtimeInfo.manifest }
if ($ReplayEyeOffset) { [IO.File]::WriteAllText((Join-Path $run 'eye-textures.request'),'Own two native geometry textures from one scene and verify their exact source hashes after restoration.') }
$receipt.waitForClipTransition=[bool]$WaitForClipTransition
$receipt.traceReplaySamples=[bool]$TraceReplaySamples
$receipt.verifyFreshReplayColor=[bool]$VerifyFreshReplayColor
$receipt.repeatAoDraw=[bool]$RepeatAoDraw
$receipt.repeatShadowDraw=[bool]$RepeatShadowDraw
$receipt.normalizeShadowBindings=[bool]$NormalizeShadowBindings
$receipt.isolateReplayModels=[bool]$IsolateReplayModels
$receipt.restoreReplayComputeState=[bool]$RestoreReplayComputeState
$receipt.sortReplayVpl=[bool]$SortReplayVpl
$receipt.isolateReplayShadowSampler=[bool]$IsolateReplayShadowSampler
$receipt.keepCaptures=[bool]$KeepCaptures
$timer=[Diagnostics.Stopwatch]::StartNew()
$seen=@{}; $handles=@{}; $captureTimes=if($RunKind -eq 'Probe'){@($CaptureAfterSeconds)}else{@($CaptureAfterSeconds,($CaptureAfterSeconds+30),($CaptureAfterSeconds+60))}; $requested=@{}
try {
    Write-Output "Diagnostic evidence: $run"
    if($practiceFlow) { Install-PracticeFlow $game $run $practiceFlow }
    if($cockpitCamera) { Install-CockpitCamera $game $run $cockpitCamera }
    if($cockpitAssets.Count) { Install-CockpitAssets $game $run $cockpitAssets }
    if($cinemaMatte) { Install-CinemaMatte $game $run $cinemaMatte }
    if($instanceDeployment) { Install-InstanceCapacity $game $run $instanceDeployment }
    foreach ($entry in $entries) {
        # Copy with overwrite disabled, including if another process raced preflight.
        [IO.File]::Copy($entry.source,(Join-Path $game $entry.name),$false)
        if ((Get-FileHash -LiteralPath (Join-Path $game $entry.name)).Hash -ne $entry.sha256) { throw "Deployed file changed before launch: $($entry.name)" }
    }
    if ($selectedSession -and (Get-FileHash -LiteralPath $selectedSession.configuration).Hash -ne $selectedSession.configurationSha256) { throw 'Staged session changed before launch.' }
    Start-Process -FilePath (Join-Path (Get-Grid2SteamRoot) 'steam.exe') -ArgumentList $steamArgumentLine -WindowStyle Hidden | Out-Null
    $stereoReadyReported=$false
    $captureCompleteAt=$null
    while ($UntilExit -or $timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $processes=@(Get-CimInstance Win32_Process -Filter "Name='grid2.exe' OR Name='grid2_avx.exe'")
        foreach ($p in $processes) {
            if (!$seen.ContainsKey([int]$p.ProcessId)) {
                if ($p.ExecutablePath -ne (Join-Path $game $p.Name) -or $p.CreationDate.ToUniversalTime().Ticks -lt ([datetimeoffset]$receipt.startedUtc).UtcTicks) { continue }
                $identity=@{path=$p.ExecutablePath;createdUtc=$p.CreationDate.ToUniversalTime().ToString('o')}
                $seen[[int]$p.ProcessId]=$identity
                $process=Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
                if ($process) { $null=$process.Handle; $handles[[int]$p.ProcessId]=$process }
                $receipt.processes+=@{id=$p.ProcessId;parent=$p.ParentProcessId;path=$p.ExecutablePath;commandLine=$p.CommandLine;createdUtc=$identity.createdUtc}
                if ($UntilExit -or $RecenterDeviceBase64) { $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'session.json') }
                if($RecenterDeviceBase64 -and $RuntimeSession -and $XrProjection -and $process) {
                    try { Start-RecenterDevice $root $run $RecenterDeviceBase64 $process }
                    catch { [IO.File]::WriteAllText((Join-Path $run 'recenter-controller.json'),(@{result=$_.Exception.Message;requests=0} | ConvertTo-Json)); Write-Warning "Device recenter unavailable: $_" }
                }
                Write-Output "Observed $($p.Name), PID $($p.ProcessId)"
            }
        }
        if($career.id -ne 'current' -and (Test-Path -LiteralPath (Join-Path $run 'profile-storage.json'))) {
            $careerState=$null
            try { $careerState=Read-ProfileJson (Join-Path $run 'profile-storage.json') } catch { }
            if($careerState.phase -eq 'refused') { $receipt.careerFailure=$careerState.reason; $receipt.result='profile_storage_refused'; break }
        }
        if ($ProxyMode -eq 'Active' -and !$PlayerControlOnly) {
            foreach ($seconds in $captureTimes) {
                if (!$requested.ContainsKey($seconds) -and $timer.Elapsed.TotalSeconds -ge $seconds) {
                    New-Item -ItemType File -Path (Join-Path $run 'capture.request') -Force | Out-Null
                    $requested[$seconds]=$true
                }
            }
        }
        if ($RunKind -eq 'Probe' -and (Test-Path -LiteralPath (Join-Path $run 'probe-complete'))) {
            $receipt.result='probe_window_completed'; break
        }
        if($AutoCaptureFlicker -and $seen.Count -gt 0 -and $processes.Count -gt 0) {
            $decision=Get-FlickerAutomationDecision $run $FlickerCaptureAfterPairs $receipt.automaticFlicker.requested
            if($decision -eq 'capture') {
                # Deployment and process ownership are established above. The
                # render thread consumes this once; there is no input/focus change.
                [IO.File]::WriteAllText((Join-Path $run 'flicker-capture.request'),'Automatic capture after healthy stereo warmup.')
                $receipt.automaticFlicker.requested=$true
                $receipt.automaticFlicker.requestedSeconds=$timer.Elapsed.TotalSeconds
                $receipt.automaticFlicker.outcome='capturing'
                Write-Output "Automatic flicker capture requested: $run"
            } elseif($decision -ne 'wait') {
                if($decision -eq 'complete' -and $FlickerObservationSeconds) {
                    if($null -eq $captureCompleteAt) {
                        $captureCompleteAt=$timer.Elapsed.TotalSeconds
                        Write-Output "Capture complete; continuing bounded observation for $FlickerObservationSeconds seconds without more images."
                    }
                    $receipt.automaticFlicker.observedSeconds=$timer.Elapsed.TotalSeconds-$captureCompleteAt
                    if($receipt.automaticFlicker.observedSeconds -lt $FlickerObservationSeconds) {
                        $receipt.automaticFlicker.outcome='observing'
                        Start-Sleep -Milliseconds 500
                        continue
                    }
                }
                $receipt.automaticFlicker.outcome=$decision
                $receipt.result='probe_window_completed'
                Write-Output "Automatic flicker capture finished: $decision"
                break
            }
        }
        if ($WaitForStereoStart -and !$stereoReadyReported -and (Test-Path -LiteralPath (Join-Path $run 'stereo-ready.txt'))) {
            Write-Output "Stereo waiting: $run; confirm the game is unpaused, then create stereo-start.request here."
            $stereoReadyReported=$true
        }
        if ($seen.Count -gt 0 -and !$processes.Count) { $receipt.result='process_exited'; break }
        if ($UntilExit -and (Test-Path -LiteralPath (Join-Path $run 'session-exit.request'))) { $receipt.result='session_end_requested'; break }
        if (!$seen.Count -and $timer.Elapsed.TotalSeconds -gt 60) { $receipt.result='no_game_process'; break }
        Start-Sleep -Milliseconds 500
    }
    if ($receipt.result -in @('prepared','probe_window_completed','session_end_requested','profile_storage_refused')) {
        if ($receipt.result -eq 'prepared') { $receipt.result='timeout_owned_game_terminated' }
        if ($RuntimeSession -and $seen.Count -gt 0) {
            # Ask the render thread to finish restoration and release XR first.
            # This is an owned runtime message, not simulated game/window input.
            [IO.File]::WriteAllText((Join-Path $run 'runtime-stop.request'),'Stop rendering; leave the native game running until owned-process recovery.')
            $stopTimer=[Diagnostics.Stopwatch]::StartNew()
            while ($stopTimer.Elapsed.TotalSeconds -lt 5 -and !(Test-Path -LiteralPath (Join-Path $run 'runtime-stopped'))) { Start-Sleep -Milliseconds 100 }
            $receipt.runtimeStopAcknowledged=Test-Path -LiteralPath (Join-Path $run 'runtime-stopped')
        }
        foreach ($id in $seen.Keys) {
            $current=Get-CimInstance Win32_Process -Filter "ProcessId=$id"
            $p=Get-Process -Id $id -ErrorAction SilentlyContinue
            if ($current -and $p -and $current.ExecutablePath -eq $seen[$id].path -and $p.Path -eq $seen[$id].path -and $current.CreationDate.ToUniversalTime().ToString('o') -eq $seen[$id].createdUtc) {
                Stop-Process -InputObject $p
                if (!$p.WaitForExit(10000)) { throw "Owned diagnostic process $id did not exit." }
                $receipt.terminatedOwnedProcessIds+=@($id)
            }
        }
    }
} catch {
    $receipt.error=$_.Exception.ToString()
    $receipt.errorStack=$_.ScriptStackTrace
    throw
} finally {
    try {
        $receipt.elapsedSeconds=$timer.Elapsed.TotalSeconds
        $receipt.exits=@(foreach($id in $handles.Keys){$process=$handles[$id]; if($process.HasExited){@{id=$id;code=$process.ExitCode}}})
        $faultFile=Join-Path $run 'engine-faults.txt'
        $receipt.engineExceptionObserved=(Test-Path -LiteralPath $faultFile) -and (Get-Item -LiteralPath $faultFile).Length -gt 0
        $receipt.documentResults=@($before | ForEach-Object {$after=if(Test-Path -LiteralPath $_.path){(Get-FileHash -LiteralPath $_.path).Hash}else{$null};@{path=$_.path;before=$_.sha256;after=$after;unchanged=($after -eq $_.sha256)}})
        $documents=Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\GRID 2'
        & (Join-Path $PSScriptRoot 'Write-DiagnosticReceipt.ps1') -Receipt $receipt -Run $run -Documents $documents
    } finally {
        foreach($process in $handles.Values){$process.Dispose()}
        try { Restore-Diagnostic } catch {
            @{error=$_.Exception.ToString();stack=$_.ScriptStackTrace} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'recovery-error.json')
            throw
        }
    }
    if(!$DeferCaptureFinalization) {
        try {
            & (Join-Path $PSScriptRoot 'Finish-Diagnostic.ps1') -RunPath $run -KeepCaptures:$KeepCaptures -PythonPath $PythonPath
        } catch {
            Write-Warning "Capture cleanup deferred: $($_.Exception.Message)"
        }
    }
    Write-Output "Result: $($receipt.result); evidence: $run"
}

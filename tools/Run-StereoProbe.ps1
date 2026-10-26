# One bounded desktop rehearsal or headset projection test. Preflight is read-only.
param(
    [ValidateSet('Preflight','Desktop','Headset')][string]$Mode='Preflight',
    [ValidateSet('VDXR','SteamVR')][string]$XrRuntime='VDXR',
    [ValidateSet(25,50,75,100,125,150,175,200)][int]$HeadsetScale=100,
    [ValidateRange(2,900)][int]$Frames=900,
    [ValidateRange(1,300)][int]$CaptureAfterSeconds=75,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180,
    [switch]$KeepCaptures,
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
    [switch]$ForceSceneResolution,
    [switch]$FlickerDiagnostic,
    [switch]$AutoCaptureFlicker,
    [switch]$FlickerSignTarget,
    [switch]$NativeVisibilityControl,
    [switch]$TrackedVisibility,
    [switch]$HeadsetVisibilityCandidate,
    [switch]$MirrorPreview,
    [switch]$VisibilityCapture,
    [switch]$ScreenUiStereo,
    [switch]$FrontendSceneAudit,
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
    [string]$HardwareSettingsFile=(Join-Path $PSScriptRoot 'stereo-probe-settings.xml'),
    [string]$RecordedViewsFile='',
    [string]$SessionDirectory='',
    [switch]$NormalLaunch,
    [string]$PythonPath=''
)
$ErrorActionPreference='Stop'
if($NormalLaunch -and ($SessionDirectory -or !$RuntimeSession -or $PlayerControlProbe -ne 'Off')) { throw 'Normal launch requires the guarded runtime without a selected session or input probe.' }
$RemoveCinemaMatte=$RemoveCinemaMatte -or ($Mode -eq 'Headset' -and $RuntimeSession)
if($MsaaAudit -and ($Mode -notin @('Desktop','Preflight') -or !$RuntimeSession -or !$FlickerDiagnostic -or !$DiagnosticProxy -or $UntilExit)) { throw 'MSAA audit requires an isolated bounded desktop diagnostic.' }
if($ExposureAudit -and ($Mode -notin @('Desktop','Preflight') -or !$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or !$DiagnosticProxy -or $UntilExit -or $NormalLaunch)) { throw 'Exposure audit requires an isolated bounded unattended desktop diagnostic.' }
if($ScreenUiStereo -and (!$RuntimeSession -or !$FlickerDiagnostic -or !$DiagnosticProxy -or $PlayerControlOnly)) { throw 'UI stereo candidate requires an explicit diagnostic renderer and a stereo runtime session.' }
if($FlickerObservationSeconds -and ($Mode -ne 'Desktop' -or !$TrackedVisibility -or !$VisibilityCapture -or !$AutoCaptureFlicker -or !$RuntimeSession -or $UntilExit -or $HeadsetVisibilityCandidate -or $CaptureAfterSeconds+$FlickerObservationSeconds+20 -ge $TimeoutSeconds)) { throw 'Extended observation requires bounded simulated tracked visibility with reduced capture and enough timeout.' }
if($HeadsetVisibilityCandidate -and ($Mode -notin @('Preflight','Headset') -or !$RuntimeSession -or !$UntilExit -or !$FlickerDiagnostic -or !$TrackedVisibility -or !$MirrorPreview -or !$VisibilityCapture -or $InstanceCapacity -ne 'Expand' -or !$DiagnosticProxy -or $AutoCaptureFlicker -or $RecordedViewsFile -or $NativeVisibilityControl -or $FlickerSignTarget -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off' -or $WaitForStereoStart)) { throw 'Headset visibility candidate requires isolated live-XR human practice with qualified capacity and no recorded/automatic capture.' }
if($InstanceCapacity -ne 'Off' -and (!$TrackedVisibility -or !$VisibilityCapture)) { throw 'Instance-capacity diagnostics require tracked visibility and reduced capture.' }
if($VisibilityCapture -and ((!$TrackedVisibility -and !$NativeVisibilityControl) -or $FlickerSignTarget)) { throw 'Visibility-only capture requires a tracked or native visibility diagnostic without sign-target capture.' }
if($MirrorPreview -and !$TrackedVisibility) { throw 'Mirror preview requires the isolated tracked-visibility diagnostic.' }
if($TrackedVisibility -and !$HeadsetVisibilityCandidate -and ($NativeVisibilityControl -or $Mode -ne 'Desktop' -or !$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or $UntilExit -or $WaitForStereoStart -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off')) { throw 'Tracked visibility requires exclusive bounded unattended simulated AI diagnostics.' }
if($NativeVisibilityControl -and ($Mode -ne 'Desktop' -or !$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or $UntilExit -or $WaitForStereoStart -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off')) { throw 'Native-visibility control requires bounded unattended simulated AI diagnostics.' }
if($FlickerSignTarget) {
    if($Mode -ne 'Desktop' -or !$RuntimeSession -or !$FlickerDiagnostic -or !$AutoCaptureFlicker -or $UntilExit -or $WaitForStereoStart -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off') { throw 'Sign-target capture requires unattended simulated AI flicker diagnostics.' }
    if($PSBoundParameters.ContainsKey('FlickerCaptureAfterPairs') -and $FlickerCaptureAfterPairs -ne 1) { throw 'Sign-target capture must be requested after the first stereo pair.' }
    $FlickerCaptureAfterPairs=1
}
if($RemoveCinemaMatte -and (!$RuntimeSession -or $PlayerControlOnly -or ($Mode -eq 'Desktop' -and
    (!$FlickerDiagnostic -or !$DiagnosticProxy -or !$AutoCaptureFlicker -or $UntilExit)))) {
    throw 'Cinema matte removal requires VR or an explicit bounded simulated-eye diagnostic.'
}
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'Recenter-Key.ps1')
. (Join-Path $PSScriptRoot 'Headset-Settings.ps1')
. (Join-Path $PSScriptRoot 'Flicker-Automation.ps1')
Assert-RecenterKey $RecenterKey $RecenterModifiers
. (Join-Path $PSScriptRoot 'Recenter-Device.ps1')
Assert-RecenterDevice (Split-Path $PSScriptRoot -Parent) $RecenterDeviceBase64
$artifacts=Get-Grid2DataRoot $root
$game=Get-Grid2GameRoot $root
$steam=Get-Grid2SteamRoot
. (Join-Path $PSScriptRoot 'OpenXR-Runtime.ps1')
. (Join-Path $PSScriptRoot 'User-Session.ps1')
$runtime=Get-Grid2VrRuntimeManifest $XrRuntime
$proxy=Get-Grid2Proxy $root
if($DiagnosticProxy) {
    if(!$FlickerDiagnostic) { throw 'A diagnostic proxy override requires FlickerDiagnostic.' }
    $proxy=(Resolve-Path -LiteralPath $DiagnosticProxy).Path
}
if($FlickerDiagnostic -and (!$RuntimeSession -or $PlayerControlOnly)) { throw 'Flicker capture requires the stereo runtime.' }
if($AutoCaptureFlicker -and (!$FlickerDiagnostic -or $UntilExit)) { throw 'Automatic flicker capture requires a bounded diagnostic.' }
$documents=Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\GRID 2\hardwaresettings'
$sessionJournal=Join-Path $artifacts 'session-pending.json'
if (Test-Path -LiteralPath $sessionJournal) { throw 'Recover the pending user session before starting another run.' }
if ($UntilExit -and (!$RuntimeSession -or ($Mode -eq 'Desktop' -and !$PlayerControlOnly))) { throw 'User sessions require Headset rendering or non-rendering Desktop practice.' }
if ($CaptureAfterSeconds -ge $TimeoutSeconds) { throw 'Capture delay must be less than timeout.' }
if ($RecordedViewsFile -and $Mode -ne 'Desktop') { throw 'Recorded input is only supported in Desktop mode.' }
if ($RuntimeSession -and ($WaitForStereoStart -or $KeepCaptures)) { throw 'Runtime sessions do not use the diagnostic chat gate or capture retention.' }
if ($PlayerControlOnly -and (($PlayerControlProbe -eq 'Off' -and $BackgroundPlay -eq 'Off') -or $RecordedViewsFile)) { throw 'Non-rendering checks require a player control or background-play probe and no recorded headset views.' }
if ($BackgroundPlay -ne 'Off' -and !$RuntimeSession) { throw 'Background play requires a guarded runtime session.' }
if (Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing game session retained.' }
if (Test-Path -LiteralPath (Join-Path $artifacts 'diagnostic-pending.json')) { throw 'Restore the pending diagnostic first.' }
if (Test-Path -LiteralPath (Join-Path $artifacts 'practice-flow-pending.json')) { throw 'Restore the earlier experimental practice flow first.' }
if (Test-Path -LiteralPath (Join-Path $artifacts 'steamvr-theater-pending.json')) { throw 'Restore the pending SteamVR theater preference with tools/steamvr_theater.py restore first.' }
foreach($name in @('d3d11.dll','dxgi.dll','GRID2VR.diagnostic-session','GRID2VR.session.xml')) {
    if(Test-Path -LiteralPath (Join-Path $game $name)) { throw "Existing $name retained; preflight refused." }
}
$expected=@{
    'grid2_avx.exe'='519CD994D2FF92A2A7A2A784856854183F53E359B83F8B9563CC09A0AE91E502'
    'grid2.exe'='F92C2E1134B5D6D61404131224B41C182089BF563D28731EEF0FAA7D8096AB4C'
}
foreach($name in $expected.Keys) {
    if((Get-FileHash -LiteralPath (Join-Path $game $name)).Hash -ne $expected[$name]) { throw "Unsupported $name." }
}
# Desktop runs need no installed headset runtime. Preflight validates the selection.
if($UntilExit -and $Mode -eq 'Headset') { Write-Output 'GRID2VR_LAUNCH|Checking the selected OpenXR runtime...' }
$runtimeInfo=if($Mode -ne 'Desktop'){Resolve-Grid2VrRuntime $runtime}else{$null}
$suppressTheater=$Mode -eq 'Headset' -and $runtimeInfo.theaterSuppression
if(!(Test-Grid2Package $root)) {
    $cache=Get-Content -LiteralPath (Join-Path $root 'build/win32/CMakeCache.txt') -Raw
    if($cache -notmatch '(?m)^GRID2VR_ENABLE_XR_SESSION:BOOL=ON\r?$') { throw 'Build with GRID2VR_ENABLE_XR_SESSION=ON first.' }
}
if(!$PythonPath) { $PythonPath=Get-Grid2Python $root }
if(!(Test-Path -LiteralPath $PythonPath -PathType Leaf)) { throw 'Python executable is missing.' }
$selectedSession=$null
if($SessionDirectory) {
    $verified=& $PythonPath (Join-Path $PSScriptRoot 'session_config.py') --game-root $game verify --session $SessionDirectory
    if($LASTEXITCODE) { throw 'Selected session validation failed; nothing deployed.' }
    $selectedSession=$verified | ConvertFrom-Json
}
if ($PlayerControlProbe -ne 'Off' -and ($Mode -notin @('Preflight','Desktop') -or !$RuntimeSession -or !$selectedSession -or $selectedSession.selection.mode -ne 'attract')) { throw 'Player input probes require a selected solo attract session and the desktop runtime.' }
if ($selectedSession -and $selectedSession.control -eq 'human' -and (!$RuntimeSession -or $PlayerControlProbe -ne 'Off')) { throw 'Human practice uses the guarded runtime and native controls, with the old input probe off.' }
if ($UntilExit -and !$NormalLaunch -and (!$selectedSession -or $selectedSession.control -ne 'human')) { throw 'User sessions require selected human practice/race or explicit Normal launch.' }
if($AutoCaptureFlicker -and (!$selectedSession -or $selectedSession.control -ne 'ai' -or $selectedSession.selection.mode -notin @('attract','benchmark'))) { throw 'Automatic flicker capture requires selected AI playback.' }
if($CockpitS2000 -ne 'Off') {
    if(!$RuntimeSession -or !$UntilExit -or $PlayerControlProbe -ne 'Off' -or (!$NormalLaunch -and $selectedSession.selection.car -cne 's20')) { throw 'Cockpit prototype requires a user-run S2000 session or Normal mode.' }
    . (Join-Path $PSScriptRoot 'Cockpit-Camera.ps1')
    Assert-CockpitCameraOriginal $game
    $null=Get-CockpitCameraCandidate $root $CockpitS2000
}
$cockpitAssets=@()
$cockpitModeRequested=$CockpitMode
if($selectedSession.carContent.dlc -and !$selectedSession.carContent.cockpitAvailable) { $CockpitMode='Off' }
if($CockpitMode -ne 'Off') {
    if($UntilExit -and $Mode -ne 'Preflight') { Write-Output 'GRID2VR_LAUNCH|Checking cockpit cameras and car assets...' }
    if(!$RuntimeSession -or (!$UntilExit -and !$ExposureAudit) -or $PlayerControlOnly -or $PlayerControlProbe -ne 'Off' -or $CockpitS2000 -ne 'Off' -or (!$ExposureAudit -and $Mode -notin @('Preflight','Headset'))) { throw 'Launcher cockpit assets require a VR user session or bounded exposure audit.' }
    . (Join-Path $PSScriptRoot 'Cockpit-Assets.ps1')
    $cockpitAssets=@(Get-CockpitAssetSelection $root $game $CockpitMode $selectedSession.selection.car ([bool]$NormalLaunch))
}
$protectedPaths=@((Join-Path $documents 'hardware_settings_config.xml'),(Join-Path $documents 'hardware_settings_info.xml'))
foreach($account in Get-ChildItem -LiteralPath (Join-Path $steam 'userdata') -Directory) {
    $saveRoot=Join-Path $account.FullName '44350'
    if(Test-Path -LiteralPath $saveRoot) {
        if((Get-Item -LiteralPath $saveRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked save root is unsupported.' }
        $items=@(Get-ChildItem -LiteralPath $saveRoot -Recurse -Force)
        if(@($items | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked save data is unsupported.' }
        $protectedPaths+=@($items | Where-Object { !$_.PSIsContainer } | ForEach-Object { $_.FullName })
    }
}
if($RemoveCinemaMatte) { $protectedPaths+=Join-Path $game 'frontend/databases/overlayPersistent.pssg' }
if($CockpitS2000 -ne 'Off') { $protectedPaths+=Join-Path $game 'cars/models/s20/cameras.xml' }
foreach($camera in $cockpitAssets) { $protectedPaths+=Join-Path $game $camera.name }
$protectedPaths=@($protectedPaths | Sort-Object -Unique)
foreach($path in @($game,$documents,$hardwareSettingsFile,$proxy)+$protectedPaths) {
    if((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked input retained: $path" }
}
$protectedBytes=($protectedPaths | ForEach-Object { (Get-Item -LiteralPath $_).Length } | Measure-Object -Sum).Sum
if($protectedPaths.Count -gt 1000 -or $protectedBytes -gt 100MB) { throw 'Protected input set exceeds the diagnostic backup bound.' }
$preflight=[ordered]@{mode=$Mode;frames=$Frames;captureAfterSeconds=$CaptureAfterSeconds;waitForStereoStart=[bool]$WaitForStereoStart;timeoutSeconds=$TimeoutSeconds;proxySha256=(Get-FileHash -LiteralPath $proxy).Hash;protectedFiles=$protectedPaths.Count;protectedBytes=$protectedBytes;runtime=$runtime;runtimeOpened=$false;uiAutomation=$false}
if($selectedSession) { $preflight.selectedSession=$selectedSession }
$preflight.runtimeSession=[bool]$RuntimeSession
$preflight.normalLaunch=[bool]$NormalLaunch
$preflight.cockpitS2000=$CockpitS2000
$preflight.cockpitMode=$CockpitMode
$preflight.cockpitModeRequested=$cockpitModeRequested
$preflight.cockpitApplied=($cockpitAssets.Count -gt 0)
$preflight.cockpitExposureLimit=($CockpitMode -ne 'Off' -and !$NativeCockpitExposure)
$preflight.exposureAudit=[bool]$ExposureAudit
$preflight.cockpitAssets=$cockpitAssets
$preflight.skipIntro=[bool]$SkipIntro
$preflight.removeCinemaMatte=[bool]$RemoveCinemaMatte
if($RemoveCinemaMatte) {
    . (Join-Path $PSScriptRoot 'Cinema-Matte.ps1')
    Assert-CinemaMatteOriginal $game
}
if($SkipIntro) {
    . (Join-Path $PSScriptRoot 'Native-Startup.ps1')
    Assert-NativeStartup $game
}
$preflight.diagnosticLogging=[bool]$DiagnosticLogging
. (Join-Path $PSScriptRoot 'Driving-Controls.ps1')
$validatedDrivingControls=Read-DrivingControls $DrivingControls
$preflight.drivingControls=if($validatedDrivingControls){@{sha256=$validatedDrivingControls.sha256;bindings=$validatedDrivingControls.bindings;sessionOnly=$true}}else{$null}
$preflight.vrHud=@{enabled=([bool]$RuntimeSession -and !$PlayerControlOnly);distance=$HudDistance;followView=[bool]$HudFollowView;hiddenElements=$HudHiddenElements}
$preflight.vrReplayCameras=[bool]$VrReplayCameras
$preflight.vrGarageMenus=[bool]$VrGarageMenus
$preflight.frontendSceneAudit=[bool]$FrontendSceneAudit
$preflight.performanceDisplay=[bool]$PerformanceDisplay
$preflight.performanceFollowView=[bool]$PerformanceFollowView
$preflight.msaaAudit=[bool]$MsaaAudit
$sceneWidth=0; $sceneHeight=0
if($ForceSceneResolution) {
    $sceneResolution=([xml][IO.File]::ReadAllText($HardwareSettingsFile)).hardware_settings_config.graphics_card.resolution
    $sceneWidth=[int]$sceneResolution.width; $sceneHeight=[int]$sceneResolution.height
    if(!$RuntimeSession -or $PlayerControlOnly -or $sceneWidth -lt 640 -or $sceneWidth -gt 3840 -or $sceneHeight -lt 360 -or $sceneHeight -gt 2160) { throw 'Invalid native VR scene-size request.' }
}
$preflight.nativeSceneResolution=@{enabled=[bool]$ForceSceneResolution;width=$sceneWidth;height=$sceneHeight}
. (Join-Path $PSScriptRoot 'Profiles.ps1')
$career=Get-LaunchProfile -Root $root -Id $CareerProfile
$preflight.career=$career
if($career.id -ne 'current') {
    $careerProxy=Get-CareerProfileProxy -Root $root -BaseProxy $proxy
    $preflight.proxySha256=(Get-FileHash -LiteralPath $careerProxy).Hash
}
$preflight.flickerDiagnostic=[bool]$FlickerDiagnostic
$preflight.autoCaptureFlicker=[bool]$AutoCaptureFlicker
$preflight.flickerSignTarget=[bool]$FlickerSignTarget
$preflight.nativeVisibilityControl=[bool]$NativeVisibilityControl
$preflight.trackedVisibility=[bool]$TrackedVisibility
$preflight.headsetVisibilityCandidate=[bool]$HeadsetVisibilityCandidate
$preflight.mirrorPreview=[bool]$MirrorPreview
$preflight.visibilityCapture=[bool]$VisibilityCapture
$preflight.screenUiStereo=[bool]$ScreenUiStereo
$preflight.instanceCapacity=$InstanceCapacity
$preflight.flickerCaptureAfterPairs=$FlickerCaptureAfterPairs
$preflight.flickerObservationSeconds=$FlickerObservationSeconds
if($HardwareSettingsFile) {
    $preflight.hardwareSettingsFile=(Get-Item -LiteralPath $HardwareSettingsFile).FullName
    $preflight.hardwareSettingsSha256=(Get-FileHash -LiteralPath $HardwareSettingsFile).Hash
}
$preflight.recenterKey=$RecenterKey
$preflight.recenterModifiers=$RecenterModifiers
$preflight.keepHorizonLevel=!$RecenterTilt
$preflight.untilExit=[bool]$UntilExit
$preflight.playerControlProbe=$PlayerControlProbe
$preflight.playerControlOnly=[bool]$PlayerControlOnly
$preflight.desktopBorderlessRequested=([bool]$RuntimeSession -and [bool]$UntilExit -and [bool]$PlayerControlOnly)
$preflight.backgroundPlay=$BackgroundPlay
$preflight.xrRuntime=$XrRuntime
$preflight.headsetSettings=Get-HeadsetSettings $HeadsetScale
$preflight.runtimeInfo=$runtimeInfo
$preflight | ConvertTo-Json -Depth 6
if($Mode -eq 'Preflight') { return }
$backup=Join-Path $artifacts ('stereo-probe-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup | Out-Null
$preflight | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $backup 'preflight.json')
$files=@(); $i=0
if($UntilExit) { Write-Output 'GRID2VR_LAUNCH|Backing up original settings and protected files...' }
foreach($path in $protectedPaths) {
    $target=Join-Path $backup ("protected-"+($i++)+'.bin')
    [IO.File]::Copy($path,$target,$false)
    $hash=(Get-FileHash -LiteralPath $target).Hash
    if((Get-FileHash -LiteralPath $path).Hash -ne $hash) { throw 'Protected file changed while saving backup.' }
    $files+=@{path=$path;backup=$target;before=$hash}
}
$executables=@($expected.Keys | ForEach-Object { @{path=(Join-Path $game $_);before=$expected[$_]} })
@{files=$files;executables=$executables} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $backup 'manifest.json')
if ($UntilExit) {
    $worker=Get-Process -Id $PID
    $userSession=@{schema=1;backup=$backup;run=$null;workerId=$PID;workerStartedUtc=$worker.StartTime.ToUniversalTime().ToString('o');mode=$Mode;runtime=$XrRuntime}
    Write-SessionJson $sessionJournal $userSession
}
if($Mode -eq 'Desktop' -and !$RecordedViewsFile -and !$PlayerControlOnly) {
    $recordedViewsFile=Join-Path $backup 'recorded-views.txt'
    & $PythonPath (Join-Path $PSScriptRoot 'make_recorded_views.py') $recordedViewsFile --count $Frames
    if($LASTEXITCODE) { throw 'Recorded input generation failed.' }
}
$output=[Collections.Generic.List[object]]::new()
try {
    if($UntilExit) { Write-Output 'GRID2VR_LAUNCH|Applying temporary launch settings...' }
    if($suppressTheater) {
        & $PythonPath (Join-Path $PSScriptRoot 'steamvr_theater.py') begin --receipt (Join-Path $backup 'theater-begin.json')
        if($LASTEXITCODE) { throw 'SteamVR automatic-theater suppression failed; game not launched.' }
    }
    if($HardwareSettingsFile) {
        $settings=@($files | Where-Object { $_.path.EndsWith('\hardware_settings_config.xml') })
        if($settings.Count -ne 1 -or (Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue)) { throw 'Temporary settings require one protected config and no game process.' }
        $settings=$settings[0]
        if((Get-FileHash -LiteralPath $settings.path).Hash -ne $settings.before) { throw 'Settings changed before temporary deployment.' }
        $inputPath=(Get-Item -LiteralPath $HardwareSettingsFile -ErrorAction Stop).FullName
        $inputXml=[xml][IO.File]::ReadAllText($inputPath)
        if(!$inputXml.hardware_settings_config.graphics_card.resolution) { throw 'Invalid temporary hardware settings.' }
        $inputBackup=Join-Path $backup 'probe-settings.xml'
        [IO.File]::Copy($inputPath,$inputBackup,$false)
        $settings.probeBefore=(Get-FileHash -LiteralPath $inputBackup).Hash
        @{files=$files;executables=$executables} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $backup 'manifest.json')
        [IO.File]::Copy($inputBackup,$settings.path,$true)
        if((Get-FileHash -LiteralPath $settings.path).Hash -ne $settings.probeBefore) { throw 'Temporary settings deployment hash mismatch.' }
    }
    $parameters=@{
        EngineTrace=$true;CaptureStageTargets=$true;TraceSceneBoundary=$true
        ReplayOnce=$true;TraceReplayWork=$true;VerifyFreshReplayColor=(!$RuntimeSession)
        ReplayEyeOffset=$true;ReplayEyeFrames=$Frames;ReplayAsymmetricFov=$true
        ReplayFullPose=$true;ReplayStereo=$true;CompactStereoTrace=$true;DisableMotionBlur=$true
        RecordedViewsFile=$recordedViewsFile;XrSessionProbe=($Mode -eq 'Headset');XrProjection=($Mode -eq 'Headset')
        XrRuntimeManifest=$runtime
        HeadsetScale=$HeadsetScale
        TraceVisibility=$true;ExpandVisibility=$true;RestoreReplayComputeState=$true
        SortReplayVpl=$true;IsolateReplayShadowSampler=$true;NormalizeShadowBindings=$true
        KeepCaptures=[bool]$KeepCaptures;DeferCaptureFinalization=$true
        CaptureAfterSeconds=$CaptureAfterSeconds;TimeoutSeconds=$TimeoutSeconds;PythonPath=$PythonPath
        WaitForStereoStart=[bool]$WaitForStereoStart
        SessionDirectory=$SessionDirectory;NormalLaunch=[bool]$NormalLaunch
        NativeCockpitExposure=[bool]$NativeCockpitExposure
        RuntimeSession=[bool]$RuntimeSession
        SkipIntro=[bool]$SkipIntro;RemoveCinemaMatte=[bool]$RemoveCinemaMatte;DiagnosticLogging=[bool]$DiagnosticLogging;MsaaAudit=[bool]$MsaaAudit;ExposureAudit=[bool]$ExposureAudit;DrivingControls=$DrivingControls;CockpitS2000=$CockpitS2000;CockpitMode=$CockpitMode
        HudDistance=$HudDistance;HudFollowView=$HudFollowView;HudHiddenElements=$HudHiddenElements;PerformanceDisplay=[bool]$PerformanceDisplay;PerformanceFollowView=$PerformanceFollowView
        VrReplayCameras=$VrReplayCameras;VrGarageMenus=$VrGarageMenus;FrontendSceneAudit=[bool]$FrontendSceneAudit
        SceneWidth=$sceneWidth;SceneHeight=$sceneHeight
        FlickerDiagnostic=[bool]$FlickerDiagnostic;DiagnosticProxy=$DiagnosticProxy
        AutoCaptureFlicker=[bool]$AutoCaptureFlicker;FlickerCaptureAfterPairs=$FlickerCaptureAfterPairs
        FlickerObservationSeconds=$FlickerObservationSeconds
        FlickerSignTarget=[bool]$FlickerSignTarget
        NativeVisibilityControl=[bool]$NativeVisibilityControl
        TrackedVisibility=[bool]$TrackedVisibility
        HeadsetVisibilityCandidate=[bool]$HeadsetVisibilityCandidate
        MirrorPreview=[bool]$MirrorPreview
        VisibilityCapture=[bool]$VisibilityCapture
        ScreenUiStereo=[bool]$ScreenUiStereo
        InstanceCapacity=$InstanceCapacity
        RecenterKey=$RecenterKey;RecenterModifiers=$RecenterModifiers;RecenterDeviceBase64=$RecenterDeviceBase64;RecenterTilt=[bool]$RecenterTilt
        UntilExit=[bool]$UntilExit
        PlayerControlProbe=$PlayerControlProbe
        BackgroundPlay=$BackgroundPlay
    }
    if($PlayerControlOnly) {
        $parameters=@{
            RuntimeSession=$true;PlayerControlProbe=$PlayerControlProbe;PlayerControlOnly=$true
            SkipIntro=[bool]$SkipIntro;DiagnosticLogging=[bool]$DiagnosticLogging;DrivingControls=$DrivingControls;CockpitS2000=$CockpitS2000
            RecenterKey=$RecenterKey;RecenterModifiers=$RecenterModifiers
            UntilExit=[bool]$UntilExit
            SessionDirectory=$SessionDirectory;NormalLaunch=[bool]$NormalLaunch;PythonPath=$PythonPath;DeferCaptureFinalization=$true
            CaptureAfterSeconds=$CaptureAfterSeconds;TimeoutSeconds=$TimeoutSeconds
            BackgroundPlay=$BackgroundPlay
        }
    }
    $parameters.CareerProfile=$career.id
    & (Join-Path $PSScriptRoot 'Run-Diagnostic.ps1') @parameters | ForEach-Object {
        $output.Add($_)
        if ($UntilExit -and $_ -is [string] -and $_ -like 'Diagnostic evidence: *') {
            $userSession.run=$_.Substring('Diagnostic evidence: '.Length)
            Write-SessionJson $sessionJournal $userSession
        }
        $_
    }
} catch {
    # Tee-Object -Variable loses its buffered records when the pipeline throws.
    # Retain the run identity and original failure even if recovery also refuses.
    @{error=$_.Exception.ToString();stack=$_.ScriptStackTrace;id=$_.FullyQualifiedErrorId} | ConvertTo-Json | Set-Content (Join-Path $backup 'diagnostic-error.json')
    throw
} finally {
    try {
    $match=@($output | Where-Object { $_ -like 'Diagnostic evidence: *' }) | Select-Object -First 1
    $run=if($match){$match.Substring('Diagnostic evidence: '.Length)}else{$null}
    $receipt=if($run -and (Test-Path (Join-Path $run 'session.json'))){Get-Content (Join-Path $run 'session.json') -Raw | ConvertFrom-Json}else{$null}
    $audit=@()
    foreach($entry in $files) {
        $after=(Get-FileHash -LiteralPath $entry.path).Hash
        $restored=$false
        $hardwareInfo=$entry.path.EndsWith('\hardware_settings_info.xml')
        if($after -ne $entry.before -and ($entry.path.EndsWith('\hardware_settings_config.xml') -or $hardwareInfo)) {
            $record=@($receipt.documentResults | Where-Object { $_.path -eq $entry.path })
            $probeBefore=if($entry.probeBefore){$entry.probeBefore}else{$entry.before}
            $verified=($record.Count -eq 1 -and $record[0].before -eq $probeBefore -and $record[0].after -eq $after)
            # A failure before launch has no session receipt. Restore only our exact temporary file.
            if(!$run -and $entry.probeBefore -and $after -eq $entry.probeBefore) { $verified=$true }
            if ((Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) -or !$verified -or (Get-FileHash -LiteralPath $entry.backup).Hash -ne $entry.before) { throw 'Settings restoration guard failed; retained current file.' }
            if($hardwareInfo) {
                $originalXml=[xml][IO.File]::ReadAllText($entry.backup)
                $currentXml=[xml][IO.File]::ReadAllText($entry.path)
                if(!(Test-HardwareReportRefresh $originalXml $currentXml)) { throw 'Hardware report has changes beyond detected display modes; retained current file.' }
            }
            $changedName=if($hardwareInfo){'game-written-info.xml'}else{'game-written-settings.xml'}
            [IO.File]::Copy($entry.path,(Join-Path $backup $changedName),$false)
            [IO.File]::Copy($entry.backup,$entry.path,$true)
            $restored=$true
        }
        $final=(Get-FileHash -LiteralPath $entry.path).Hash
        $audit+=@{path=$entry.path;before=$entry.before;probeBefore=$entry.probeBefore;gameWritten=$after;after=$final;restored=$restored;unchanged=($final -eq $entry.before)}
    }
    foreach($entry in $executables) { $entry.after=(Get-FileHash -LiteralPath $entry.path).Hash; $entry.unchanged=$entry.after -eq $entry.before }
    @{run=$run;files=$audit;executables=$executables;checkedUtc=[DateTime]::UtcNow.ToString('o');gameRunning=[bool](Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue);proxyPresent=(Test-Path (Join-Path $game 'd3d11.dll'));markerPresent=(Test-Path (Join-Path $game 'GRID2VR.diagnostic-session'));sessionConfigurationPresent=(Test-Path (Join-Path $game 'GRID2VR.session.xml'));pendingJournal=(Test-Path (Join-Path $artifacts 'diagnostic-pending.json'))} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $backup 'preservation-audit.json')
    Write-Output "Preservation evidence: $backup"
    # A user session may update saves, but the temporary cinema asset must
    # always return to its original bytes, including UntilExit sessions.
    $cinemaPath=Join-Path $game 'frontend/databases/overlayPersistent.pssg'
    $cockpitPath=Join-Path $game 'cars/models/s20/cameras.xml'
    $cameraPaths=@($cockpitAssets | ForEach-Object {Join-Path $game $_.name})
    $unexpected=@($audit | Where-Object {!$_.unchanged -and (!$UntilExit -or $career.id -ne 'current' -or $_.path -in (@($cinemaPath,$cockpitPath)+$cameraPaths) -or $_.path.StartsWith($documents,[StringComparison]::OrdinalIgnoreCase))})
    if($unexpected.Count -or @($executables | Where-Object {!$_.unchanged}).Count) { throw 'A protected file differs; see preservation audit.' }
    } finally {
        if($suppressTheater) {
            & $PythonPath (Join-Path $PSScriptRoot 'steamvr_theater.py') restore --receipt (Join-Path $backup 'theater-restored.json')
            if($LASTEXITCODE) { throw 'SteamVR theater restoration failed; recovery journal retained. Run tools/steamvr_theater.py restore while SteamVR is running.' }
        }
    }
}
# Restore settings before the potentially lengthy evidence analysis.
if($run) { & (Join-Path $PSScriptRoot 'Finish-Diagnostic.ps1') -RunPath $run -KeepCaptures:$KeepCaptures -PythonPath $PythonPath }
if ($UntilExit) {
    Copy-Item -LiteralPath $sessionJournal -Destination (Join-Path $backup 'user-session-restored.json')
    Remove-Item -LiteralPath $sessionJournal
}
if($RuntimeSession) {
    if(!$run) { throw 'No runtime evidence directory was produced.' }
    Assert-CareerProfileSession -Profile $career -Run $run -Root $root
    if($AutoCaptureFlicker -and $receipt.automaticFlicker.outcome -ne 'complete') { throw "Automatic flicker capture did not complete: $($receipt.automaticFlicker.outcome); evidence: $run" }
    $summary=Get-Content -LiteralPath (Join-Path $run 'runtime-summary.json') -Raw | ConvertFrom-Json
    $cleanNonRenderingExit=$PlayerControlOnly -and $PlayerControlProbe -eq 'Off' -and $summary.nonRenderingCleanExit -and $summary.backgroundPlayObservedHealthy
    if ($UntilExit -and $summary.normalUserProcessExit -and $summary.backgroundPlayObservedHealthy -and
        ($PlayerControlOnly -or $summary.lastRenderingHealthy)) {
        Write-Output "Game exited with code 0; original settings/deployment restored. Last runtime observations are healthy; a render-thread stop acknowledgment is reported separately. Evidence: $run"
        return
    }
    if (@($summary.unexpectedFaults).Count) { throw "Game exception recorded; inspect engine-faults.txt and engine-fault-context.txt in: $run" }
    # A renderer failure also prevents a clean background-stop acknowledgment;
    # report the primary failure before that dependent shutdown check.
    if(!$PlayerControlOnly -and $summary.status.failed -eq $true) { throw "VR renderer stopped on a rendering guard; inspect: $run" }
    if($AutoCaptureFlicker) {
        & $PythonPath (Join-Path $PSScriptRoot 'analyze_runtime.py') $run | Out-Null
        if($LASTEXITCODE) { throw 'Post-cleanup runtime audit failed.' }
        $summary=Get-Content -LiteralPath (Join-Path $run 'runtime-after-cleanup.json') -Raw | ConvertFrom-Json
        $capture=Get-Content -LiteralPath (Join-Path $run 'flicker-summary.json') -Raw | ConvertFrom-Json
        Assert-AutomatedFlickerResult $summary $capture
        Write-Output "Automatic capture complete: eight matched native/left/right scenes; renderer stopped and files restored. Background input and headset appearance are not acceptance claims. Evidence: $run"
        return
    }
    # Normal's background-hook activation gate is independent of stereo readiness.
    # A clean renderer/owned exit may finish with that feature unverified. Never
    # waive a failed/malformed receipt, render guard, missing stop or input probe.
    if($NormalLaunch -and $UntilExit -and $Mode -eq 'Headset' -and !$PlayerControlOnly -and
       $PlayerControlProbe -eq 'Off' -and $BackgroundPlay -eq 'Suppress' -and
       $summary.normalUserProcessExit -and $summary.rendererHealthy -and
       $summary.readiness.backgroundPlay -eq $false -and $summary.backgroundPlayVerification -eq 'not_observed') {
        Write-Output "Normal VR session ended cleanly; original settings/deployment restored. Background play was not verified (no activation receipt). Evidence: $run"
        return
    }
    if($BackgroundPlay -ne 'Off' -and !$summary.backgroundPlayProbeHealthy -and !$cleanNonRenderingExit) { throw "Background play guards did not pass; inspect: $run" }
    if(!$PlayerControlOnly -and !$summary.nativeHealthy) { throw "Runtime guards did not pass; inspect: $run" }
    if($PlayerControlProbe -ne 'Off' -and !$summary.playerControlProbeHealthy) { throw "Player input probe guards did not pass; inspect: $run" }
    if($PlayerControlOnly) {
        if($cleanNonRenderingExit -and !$summary.backgroundPlayProbeHealthy) {
            Write-Output "Non-rendering game exited with code 0 and healthy recorded background observations; no explicit runtime-stop acknowledgment. Recovery passed. Gameplay/quit intent remains manual. Evidence: $run"
            return
        }
        Write-Output "Requested non-rendering probe and restoration checks passed. Gameplay acceptance remains manual. Evidence: $run"
        return
    }
    Write-Output "Runtime reports restored stereo pairs; this is not independent image or gameplay acceptance. Evidence: $run"
    return
}
if(!$run -or $receipt.result -ne 'probe_window_completed') {
    throw "The bounded probe did not complete cleanly. See preservation evidence: $backup"
}
$faults=@(Get-Content -LiteralPath (Join-Path $run 'engine-faults.txt'))
# This handled startup exception was also present before engine hook installation
# in the short controls. Any other observed fault keeps acceptance closed.
$unexpectedFaults=@($faults | Where-Object { $_ -notmatch '^code=c0000005 .*game_rva=0084762b .*engine_installed=0$' })
if($unexpectedFaults.Count) { throw "Unexpected observed exception; inspect engine-faults.txt in: $run" }
foreach($name in @('stereo-sequence-summary.json','visibility-summary.json')) {
    $summary=Get-Content -LiteralPath (Join-Path $run $name) -Raw | ConvertFrom-Json
    if(!$summary.accepted) { throw "Diagnostic rejected by $name. Evidence: $run" }
}
if($Mode -eq 'Headset') {
    $summary=Get-Content -LiteralPath (Join-Path $run 'openxr-session-summary.json') -Raw | ConvertFrom-Json
    if(!$summary.accepted) { throw "OpenXR submission evidence rejected. Evidence: $run" }
}
if(!$KeepCaptures) {
    $cleanup=Get-Content -LiteralPath (Join-Path $run 'capture-cleanup.json') -Raw | ConvertFrom-Json
    if($cleanup.status -ne 'complete') { throw "Capture cleanup remains incomplete. Run Finish-Diagnostic.ps1 for: $run" }
}
Write-Output "Accepted $Frames bounded stereo frames; protected files restored. Headset appearance still requires user acceptance."

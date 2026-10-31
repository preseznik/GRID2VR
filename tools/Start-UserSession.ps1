param(
    [ValidateSet('VR','Desktop')][string]$Mode='VR',
    [ValidateSet('Practice','Race','Normal')][string]$LaunchMode='Practice',
    [ValidateSet('VDXR','SteamVR')][string]$XrRuntime='VDXR',
    [string]$Car='pag',
    [string]$Track='p2p/okutama/route_1',
    [ValidateRange(1,20)][int]$Laps=1,
    [ValidateRange(1,7)][int]$Opponents=7,
    [ValidateSet('same','mixed','class')][string]$OpponentCars='same',
    [ValidateRange(50,300)][int]$RenderScale=100,
    [int]$VrRenderScale=0,
    [ValidateSet(25,50,75,100,125,150,175,200)][int]$HeadsetScale=100,
    [switch]$VrMirrorBorderless,
    [ValidateSet(0,1)][int]$DesktopBorderless=1,
    [ValidateSet(0,1)][int]$DesktopVSync=1,
    [ValidateSet('off','2xmsaa','4xmsaa','8xmsaa')][string]$DesktopMsaa='off',
    [ValidateSet('off','2xmsaa','4xmsaa','8xmsaa')][string]$VrMsaa='off',
    [ValidateSet('medium','high','ultra')][string]$TreeDetail='ultra',
    [ValidateSet('medium','high','ultra')][string]$ObjectDetail='ultra',
    [ValidateSet('off','low','medium','high','ultra')][string]$CrowdDetail='ultra',
    [ValidateSet('off','low','high','ultra')][string]$TextureFiltering='ultra',
    [ValidateLength(0,4096)][string]$GraphicsQualityBase64='',
    [int]$RecenterKey=121,
    [int]$RecenterModifiers=0,
    [switch]$RecenterTilt,
    [string]$RecenterDeviceBase64="",
    [switch]$PreviewVrFixes,
    [switch]$SkipIntro,
    [switch]$DiagnosticLogging,
    [switch]$ChaseCameraObserver,
    [ValidateSet(0,1)][int]$ChaseFreeLook=0,
    [ValidateSet(0,1)][int]$ChaseKeepAngle=0,
    [ValidateSet('left','right','always')][string]$ChaseMouseMode='right',
    [ValidateRange(180,1000)][ValidateScript({ $_ % 10 -eq 0 })][int]$ChaseMaxZoomOut=180,
    [ValidateRange(25,300)][int]$ChaseMouseSensitivity=100,
    [ValidateRange(25,300)][int]$ChaseStickSensitivity=100,
    [ValidateSet(0,1)][int]$ChaseInvertVertical=0,
    [string]$DrivingControls='',
    [string]$DeviceClaim='',
    [ValidateSet('Off','HeadOnly','HideDriver')][string]$CockpitS2000='Off',
    [ValidateSet('Off','HeadOnly','HideDriver')][string]$CockpitMode='Off',
    [switch]$NativeCockpitExposure,
    [switch]$NativeImpactEffect,
    [Alias('FocusCockpit')][ValidateSet('Off','Observe','Replace')][string]$CockpitTextures='Off',
    [ValidateRange(1.0,20.0)][ValidateScript({ $_ * 2 -eq [decimal]::Truncate($_ * 2) })][decimal]$HudDistance=1,
    [ValidateSet(0,1)][int]$HudFollowView=0,
    [ValidateSet(0,1)][int]$VrReplayCameras=1,
    [ValidateSet(0,1)][int]$VrGarageMenus=1,
    [switch]$NativeLeftEye,
    [ValidateRange(0,31)][int]$HudHiddenElements=1,
    [switch]$PerformanceDisplay,
    [ValidateSet(0,1)][int]$PerformanceFollowView=1,
    [switch]$FlickerDiagnostic,
    [switch]$HeadsetVisibilityCandidate,
    [string]$DiagnosticProxy='',
    [ValidateLength(0,16384)][string]$XrLayerBypassBase64='',
    [switch]$Preflight
)
$ErrorActionPreference='Stop'
if($GraphicsQualityBase64.Length -gt 4096) { throw 'Graphics quality transport is too large.' }
if($ChaseCameraObserver -and $ChaseFreeLook) { throw 'Chase-camera controls and the passive observer cannot run together.' }
. (Join-Path $PSScriptRoot 'Chase-Camera.ps1')
$chase=Get-Grid2ChaseConfiguration -ChaseFreeLook $ChaseFreeLook -ChaseKeepAngle $ChaseKeepAngle -ChaseMouseMode $ChaseMouseMode -ChaseMaxZoomOut $ChaseMaxZoomOut -ChaseMouseSensitivity $ChaseMouseSensitivity -ChaseStickSensitivity $ChaseStickSensitivity -ChaseInvertVertical $ChaseInvertVertical
if($VrMirrorBorderless -and $Mode -ne 'VR') { throw 'Borderless VR mirror requires a VR launch.' }
if($ChaseCameraObserver -and ($Mode -ne 'Desktop' -or !$DiagnosticProxy)) { throw 'Chase observation requires Desktop and an explicit diagnostic proxy; camera writes are unavailable.' }
if($CockpitMode -ne 'Off' -and ($Mode -ne 'VR' -or $CockpitS2000 -ne 'Off')) { throw 'Launcher cockpit mode requires VR and cannot be combined with the legacy S2000 diagnostic.' }
if($CockpitS2000 -ne 'Off' -and $LaunchMode -ne 'Normal' -and $Car -cne 's20') { throw 'The cockpit prototype is for the Honda S2000 only.' }
if($PreviewVrFixes -and ($Mode -ne 'VR' -or $DiagnosticProxy -or $HeadsetVisibilityCandidate -or $FlickerDiagnostic)) { throw 'Preview VR fixes require VR and cannot be combined with diagnostic overrides.' }
if($HeadsetVisibilityCandidate -and ($Mode -ne 'VR' -or !$DiagnosticProxy)) { throw 'Headset visibility candidate requires VR and an explicit isolated proxy.' }
if($HeadsetVisibilityCandidate) { $FlickerDiagnostic=$true }
if($XrLayerBypassBase64 -and $Mode -ne 'VR') { throw 'OpenXR layer bypasses require a VR launch.' }
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'Recenter-Key.ps1')
Assert-RecenterKey $RecenterKey $RecenterModifiers
. (Join-Path $PSScriptRoot 'Recenter-Device.ps1')
Assert-RecenterDevice (Split-Path $PSScriptRoot -Parent) $RecenterDeviceBase64
. (Join-Path $PSScriptRoot 'User-Session.ps1')
. (Join-Path $PSScriptRoot 'Profiles.ps1')
$mutex=[Threading.Mutex]::new($false,'Global\GRID2VR.Session')
$locked=$false
$receipt=$null
$sessionLaps=if($Track.StartsWith('p2p/',[StringComparison]::Ordinal)){1}else{$Laps}
try {
    try {$locked=$mutex.WaitOne(0)} catch [Threading.AbandonedMutexException] {$locked=$true}
    if(!$locked) { throw 'A GRID2VR session worker is already running.' }
    if(Get-UserSession $root) { throw 'Recover the pending user session first.' }
    $runLogs=New-Grid2LogDirectory $root
    $worker=Get-Process -Id $PID
    $log=Join-Path $runLogs 'user-session.log'
    $receipt=@{schema=1;workerId=$PID;workerStartedUtc=$worker.StartTime.ToUniversalTime().ToString('o');
        mode=$Mode;launchMode=$LaunchMode;runtime=$XrRuntime;car=$Car;track=$Track;laps=$sessionLaps;opponents=$(if($LaunchMode -eq 'Race'){$Opponents}else{0});headsetScale=$HeadsetScale;previewVrFixes=[bool]$PreviewVrFixes;skipIntro=[bool]$SkipIntro;diagnosticLogging=[bool]$DiagnosticLogging;recenterKey=$RecenterKey;recenterModifiers=$RecenterModifiers;state='starting';message="Preparing $LaunchMode session";log=$log;logDirectory=$runLogs}
    if($LaunchMode -eq 'Normal') { $receipt.car=$null; $receipt.track=$null; $receipt.laps=$null }
    $receipt.opponentCars=if($LaunchMode -eq 'Race'){$OpponentCars.ToLowerInvariant()}else{$null}
    $receipt.desktopBorderlessRequested=($Mode -eq 'Desktop' -and [bool]$DesktopBorderless)
    $receipt.chaseCameraObserver=[bool]$ChaseCameraObserver
    $receipt.chaseCameraRequested=$chase.requested
    $receipt.vrMirrorBorderlessRequested=($Mode -eq 'VR' -and [bool]$VrMirrorBorderless)
    $receipt.vrHud=@{enabled=($Mode -eq 'VR');distance=$HudDistance;followView=[bool]$HudFollowView;hiddenElements=$HudHiddenElements}
    Write-UserSessionStatus $root $receipt
    $headsetResolution=$null
    $sceneArguments=@()
    $layerBypasses=@()
    if($XrLayerBypassBase64) {
        . (Join-Path $PSScriptRoot 'OpenXR-Layers.ps1')
        # Approved in the launcher. A layer registration changed since then is not bypassed.
        $layerBypasses=@(Select-Grid2VrLayerBypasses (ConvertFrom-Grid2VrLayerTransport $XrLayerBypassBase64) (Get-Grid2VrApiLayers))
        $receipt.xrLayerBypasses=@($layerBypasses | ForEach-Object { $_.layerName })
    }
    if($VrRenderScale) {
        if($Mode -ne 'VR') { throw 'Headset-relative resolution requires a VR launch.' }
        . (Join-Path $PSScriptRoot 'Headset-Resolution.ps1')
        $receipt.message='Reading connected headset resolution...'
        Write-UserSessionStatus $root $receipt
        $headset=Get-ConnectedHeadsetResolution $root $XrRuntime $layerBypasses (Join-Path $runLogs 'headset-query.txt') -ReportSuccess:$DiagnosticLogging
        $headsetResolution=Get-HeadsetResolutionPlan $headset.width $headset.height $VrRenderScale
        $receipt.headsetResolution=$headsetResolution
        $receipt.headset=$headset
        $sceneArguments=@('--scene-width',$headsetResolution.sceneWidth,'--scene-height',$headsetResolution.sceneHeight)
    }
    $career=Get-LaunchProfile $root
    $receipt.career=$career
    $receipt.cockpitS2000=$CockpitS2000
    $receipt.cockpitModeRequested=$CockpitMode
    $receipt.cockpitMode=$CockpitMode
    $receipt.cockpitTextures=$CockpitTextures
    $receipt.cockpitExposureLimit=($Mode -eq 'VR' -and $CockpitMode -ne 'Off' -and !$NativeCockpitExposure)
    $receipt.hideImpactGlitch=($Mode -eq 'VR' -and !$NativeImpactEffect)
    $receipt.vrReplayCameras=[bool]$VrReplayCameras
    $receipt.vrGarageMenus=[bool]$VrGarageMenus
    $receipt.nativeLeftEye=($Mode -eq 'VR' -and [bool]$NativeLeftEye)
    $receipt.performanceDisplay=($Mode -eq 'VR' -and [bool]$PerformanceDisplay)
    $receipt.performanceFollowView=[bool]$PerformanceFollowView
    $receipt.removeCinemaMatte=($Mode -eq 'VR')
    Write-UserSessionStatus $root $receipt
    if($PreviewVrFixes) {
        $receipt.message='Checking the VR renderer...'
        Write-UserSessionStatus $root $receipt
        . (Join-Path $PSScriptRoot 'Launcher-VrPreview.ps1')
        # Texture comparisons must use the same verified renderer in every mode.
        $DiagnosticProxy=Get-LauncherVrPreviewProxy -Root $root -HeadsetResolution:([bool]$headsetResolution)
        $HeadsetVisibilityCandidate=$true; $FlickerDiagnostic=$true
        $receipt.proxySha256=(Get-FileHash -LiteralPath $DiagnosticProxy).Hash.ToLowerInvariant()
        Write-UserSessionStatus $root $receipt
    }
    if($career.id -ne 'current') {
        $receipt.message='Preparing the selected career profile...'
        Write-UserSessionStatus $root $receipt
        $baseProxy=if($DiagnosticProxy){$DiagnosticProxy}else{Get-Grid2Proxy $root}
        $profileProxy=Get-CareerProfileProxy -Root $root -BaseProxy $baseProxy
        $receipt.proxySha256=(Get-FileHash -LiteralPath $profileProxy).Hash.ToLowerInvariant()
        Write-UserSessionStatus $root $receipt
    }
    $python=Get-SessionPython $root
    $selection=Join-Path $runLogs 'selection'
    $receipt.message=if($LaunchMode -eq 'Normal'){'Preparing the game menus...'}else{'Preparing the selected car, track and race options...'}
    Write-UserSessionStatus $root $receipt
    if($LaunchMode -eq 'Normal') {
        # Own only graphics files; Normal uses native menus and no selected flow.
        New-Item -ItemType Directory -Path $selection -ErrorAction Stop | Out-Null
    } else {
        $bundle=Join-Path (Get-Grid2DataRoot $root) 'practice-flow-20261014'
        $selectionArguments=@('--game-root',(Get-Grid2GameRoot $root),'prepare','--output',$selection,'--car',$Car,'--track',$Track,'--laps',$sessionLaps,
            '--mode',$LaunchMode.ToLowerInvariant(),'--control','human','--practice-bundle',$bundle)
        if($LaunchMode -eq 'Race') { $selectionArguments+=@('--opponents',$Opponents,'--opponent-cars',$OpponentCars.ToLowerInvariant()) }
        $prepared=& $python (Join-Path $PSScriptRoot 'session_config.py') @selectionArguments
        if($LASTEXITCODE) { throw "$LaunchMode selection could not be prepared." }
        $prepared | Write-Output
        $content=($prepared | ConvertFrom-Json).carContent
        if($content.dlc -and !$content.cockpitAvailable) {
            $CockpitMode='Off'
            $receipt.cameraNote='DLC car uses original game cameras; custom cockpit unavailable.'
        }
    }
    $receipt.cockpitMode=$CockpitMode
    $receipt.cockpitExposureLimit=($Mode -eq 'VR' -and $CockpitMode -ne 'Off' -and !$NativeCockpitExposure)
    $hardware=Join-Path $selection 'hardware-settings.xml'
    $receipt.message='Preparing graphics settings...'
    Write-UserSessionStatus $root $receipt
    $qualityArguments=@()
    if($GraphicsQualityBase64) { $qualityArguments=@('--quality-base64',$GraphicsQualityBase64) }
    $graphics=& $python (Join-Path $PSScriptRoot 'graphics_profile.py') --output $hardware --game-root (Get-Grid2GameRoot $root) --mode $Mode --render-scale $RenderScale --desktop-vsync $DesktopVSync --desktop-msaa $DesktopMsaa --vr-msaa $VrMsaa --tree-detail $TreeDetail --object-detail $ObjectDetail --crowd-detail $CrowdDetail --texture-filtering $TextureFiltering @sceneArguments @qualityArguments
    if($LASTEXITCODE) { throw 'Graphics profile could not be prepared; nothing deployed.' }
    $receipt.graphics=$graphics | ConvertFrom-Json
    $graphics | Set-Content -LiteralPath (Join-Path $selection 'graphics-profile.json') -Encoding utf8
    $parameters=@{Mode=$(if($Preflight){'Preflight'}elseif($Mode -eq 'VR'){'Headset'}else{'Desktop'});XrRuntime=$XrRuntime;
        RunLogDirectory=$runLogs;RuntimeSession=$true;UntilExit=$true;BackgroundPlay='Suppress';RecenterKey=$RecenterKey;RecenterModifiers=$RecenterModifiers;
        # Request early; the native owner waits for its live renderer, camera,
        # visibility and tracking guards rather than a fixed race-start delay.
        HardwareSettingsFile=$hardware;HeadsetScale=$HeadsetScale;CaptureAfterSeconds=1;TimeoutSeconds=150;PythonPath=$python;
        FlickerDiagnostic=[bool]$FlickerDiagnostic;DiagnosticProxy=$DiagnosticProxy;ChaseCameraObserver=[bool]$ChaseCameraObserver;
        ForceSceneResolution=($Mode -eq 'VR');VrMirrorBorderless=[bool]$VrMirrorBorderless;DesktopBorderless=$DesktopBorderless;SkipIntro=[bool]$SkipIntro;DiagnosticLogging=[bool]$DiagnosticLogging;DrivingControls=$DrivingControls}
    if($LaunchMode -eq 'Normal') { $parameters.NormalLaunch=$true } else { $parameters.SessionDirectory=$selection }
    $parameters.DeviceClaim=$DeviceClaim
    if($layerBypasses.Count) { $parameters.XrLayerBypass=@($layerBypasses | ForEach-Object { $_.disableVariable }) }
    if($headsetResolution) {
        $parameters.HeadsetWidth=$headsetResolution.width; $parameters.HeadsetHeight=$headsetResolution.height; $parameters.VrRenderScale=$VrRenderScale
    }
    $parameters.ChaseFreeLook=$ChaseFreeLook; $parameters.ChaseKeepAngle=$ChaseKeepAngle; $parameters.ChaseMouseMode=$ChaseMouseMode
    $parameters.ChaseMaxZoomOut=$ChaseMaxZoomOut; $parameters.ChaseMouseSensitivity=$ChaseMouseSensitivity
    $parameters.ChaseStickSensitivity=$ChaseStickSensitivity; $parameters.ChaseInvertVertical=$ChaseInvertVertical
    $parameters.HudDistance=$HudDistance; $parameters.HudFollowView=$HudFollowView; $parameters.HudHiddenElements=$HudHiddenElements
    $parameters.VrReplayCameras=$VrReplayCameras
    $parameters.VrGarageMenus=$VrGarageMenus
    $parameters.NativeLeftEye=$(if($Mode -eq 'VR' -and $NativeLeftEye){'Replace'}else{'Off'})
    $parameters.PerformanceDisplay=($Mode -eq 'VR' -and [bool]$PerformanceDisplay)
    $parameters.PerformanceFollowView=$PerformanceFollowView
    $parameters.RemoveCinemaMatte=($Mode -eq 'VR')
    if($HeadsetVisibilityCandidate) {
        $parameters.HeadsetVisibilityCandidate=$true
        $parameters.TrackedVisibility=$true; $parameters.MirrorPreview=$true
        $parameters.VisibilityCapture=$true; $parameters.InstanceCapacity='Expand'
    }
    if($Mode -eq 'Desktop') { $parameters.PlayerControlOnly=$true }
    $parameters.CockpitS2000=$CockpitS2000
    $parameters.CockpitMode=$CockpitMode
    $parameters.NativeCockpitExposure=[bool]$NativeCockpitExposure
    $parameters.NativeImpactEffect=[bool]$NativeImpactEffect
    $parameters.CockpitTextures=$CockpitTextures
    if($Mode -eq 'VR') { $parameters.RecenterDeviceBase64=$RecenterDeviceBase64; $parameters.RecenterTilt=[bool]$RecenterTilt }
    $parameters.CareerProfile=$career.id
    # The worker owns its log; closing the launcher cannot close a redirected pipe
    # under a running session. Bound the log even for a user-run session.
    $receipt.message='Checking game files and launch requirements...'
    Write-UserSessionStatus $root $receipt
    & (Join-Path $PSScriptRoot 'Run-StereoProbe.ps1') @parameters | ForEach-Object {
        if($_ -is [string] -and $_.StartsWith('GRID2VR_LAUNCH|',[StringComparison]::Ordinal)) {
            $receipt.message=$_.Substring('GRID2VR_LAUNCH|'.Length)
            Write-UserSessionStatus $root $receipt
        }
        if($_ -ceq 'GRID2VR_GAME_WINDOW_READY') {
            $receipt.gameWindowReady=$true
            $receipt.message=if($Mode -eq 'VR'){'Game window open; waiting for the scene and headset...'}else{'GRID 2 window is open.'}
            Write-UserSessionStatus $root $receipt
        }
        if($_ -is [string] -and $_ -like 'Diagnostic evidence: *') {
            $receipt.run=Get-SessionRun $root $_.Substring('Diagnostic evidence: '.Length)
        }
        if(!(Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).Length -lt 128KB) {
            $line=[string]$_
            if($line.Length -gt 8192) { $line=$line.Substring(0,8192) }
            [IO.File]::AppendAllText($log,$line+[Environment]::NewLine)
        }
        $_
    }
    $receipt.state='complete'; $receipt.message=if($Preflight){if($headsetResolution){'Preflight passed; headset queried without starting a game or VR session.'}else{'Preflight passed; no game or runtime opened.'}}else{'Session ended; original files restored.'}
} catch {
    if($receipt) {
        $receipt.state='failed'; $receipt.message=$_.Exception.Message
        $failure=($_.Exception.ToString()+[Environment]::NewLine+$_.ScriptStackTrace)
        if($failure.Length -gt 16384) { $failure=$failure.Substring(0,16384) }
        if(!(Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).Length -lt 128KB) {
            [IO.File]::AppendAllText($log,$failure+[Environment]::NewLine)
        }
    }
    throw
} finally {
    try {
        if($receipt) {
            if($receipt.run) {
                $sizeReceipt=Join-Path $receipt.run 'scene-resolution.json'
                if(Test-Path -LiteralPath $sizeReceipt) { try { $receipt.resolution=Read-SessionJson $sizeReceipt } catch {} }
                $performanceReceipt=Join-Path $receipt.run 'performance-summary.json'
                if(Test-Path -LiteralPath $performanceReceipt) { try { $receipt.performance=Read-SessionJson $performanceReceipt } catch {} }
                $runtimeReceipt=Join-Path $receipt.run 'runtime-summary.json'
                if(Test-Path -LiteralPath $runtimeReceipt) {
                    try {
                        $runtime=Read-SessionJson $runtimeReceipt
                        $receipt.backgroundPlayVerification=$runtime.backgroundPlayVerification
                        if($receipt.state -eq 'complete' -and $runtime.backgroundPlayVerification -eq 'not_observed') {
                            $receipt.message+=' Background play was not verified (no activation receipt).'
                        }
                    } catch {}
                }
            }
            Write-UserSessionStatus $root $receipt
        }
    }
    finally {
        if($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}

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
    [ValidateSet(25,50,75,100,125,150,175,200)][int]$HeadsetScale=100,
    [switch]$VrMirrorBorderless,
    [ValidateSet(0,1)][int]$DesktopVSync=1,
    [ValidateSet('off','2xmsaa','4xmsaa','8xmsaa')][string]$DesktopMsaa='off',
    [ValidateSet('off','2xmsaa','4xmsaa','8xmsaa')][string]$VrMsaa='off',
    [ValidateSet('medium','high','ultra')][string]$TreeDetail='ultra',
    [ValidateSet('medium','high','ultra')][string]$ObjectDetail='ultra',
    [ValidateSet('low','medium','high','ultra')][string]$CrowdDetail='ultra',
    [ValidateSet('off','low','high','ultra')][string]$TextureFiltering='ultra',
    [int]$RecenterKey=121,
    [int]$RecenterModifiers=0,
    [switch]$RecenterTilt,
    [string]$RecenterDeviceBase64="",
    [switch]$PreviewVrFixes,
    [switch]$SkipIntro,
    [switch]$DiagnosticLogging,
    [string]$DrivingControls='',
    [ValidateSet('Off','HeadOnly','HideDriver')][string]$CockpitS2000='Off',
    [ValidateSet('Off','HeadOnly','HideDriver')][string]$CockpitMode='Off',
    [switch]$NativeCockpitExposure,
    [Alias('FocusCockpit')][ValidateSet('Off','Observe','Replace')][string]$CockpitTextures='Off',
    [ValidateRange(1.0,20.0)][ValidateScript({ $_ * 2 -eq [decimal]::Truncate($_ * 2) })][decimal]$HudDistance=1,
    [ValidateSet(0,1)][int]$HudFollowView=0,
    [ValidateSet(0,1)][int]$VrReplayCameras=1,
    [ValidateSet(0,1)][int]$VrGarageMenus=1,
    [ValidateRange(0,31)][int]$HudHiddenElements=1,
    [switch]$PerformanceDisplay,
    [ValidateSet(0,1)][int]$PerformanceFollowView=1,
    [switch]$FlickerDiagnostic,
    [switch]$HeadsetVisibilityCandidate,
    [string]$DiagnosticProxy='',
    [switch]$Preflight
)
$ErrorActionPreference='Stop'
if($VrMirrorBorderless -and $Mode -ne 'VR') { throw 'Borderless VR mirror requires a VR launch.' }
if($CockpitMode -ne 'Off' -and ($Mode -ne 'VR' -or $CockpitS2000 -ne 'Off')) { throw 'Launcher cockpit mode requires VR and cannot be combined with the legacy S2000 diagnostic.' }
if($CockpitS2000 -ne 'Off' -and $LaunchMode -ne 'Normal' -and $Car -cne 's20') { throw 'The cockpit prototype is for the Honda S2000 only.' }
if($PreviewVrFixes -and ($Mode -ne 'VR' -or $DiagnosticProxy -or $HeadsetVisibilityCandidate -or $FlickerDiagnostic)) { throw 'Preview VR fixes require VR and cannot be combined with diagnostic overrides.' }
if($HeadsetVisibilityCandidate -and ($Mode -ne 'VR' -or !$DiagnosticProxy)) { throw 'Headset visibility candidate requires VR and an explicit isolated proxy.' }
if($HeadsetVisibilityCandidate) { $FlickerDiagnostic=$true }
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
    $receipt.desktopBorderlessRequested=($Mode -eq 'Desktop')
    $receipt.vrMirrorBorderlessRequested=($Mode -eq 'VR' -and [bool]$VrMirrorBorderless)
    $receipt.vrHud=@{enabled=($Mode -eq 'VR');distance=$HudDistance;followView=[bool]$HudFollowView;hiddenElements=$HudHiddenElements}
    Write-UserSessionStatus $root $receipt
    $career=Get-LaunchProfile $root
    $receipt.career=$career
    $receipt.cockpitS2000=$CockpitS2000
    $receipt.cockpitModeRequested=$CockpitMode
    $receipt.cockpitMode=$CockpitMode
    $receipt.cockpitTextures=$CockpitTextures
    $receipt.cockpitExposureLimit=($Mode -eq 'VR' -and $CockpitMode -ne 'Off' -and !$NativeCockpitExposure)
    $receipt.vrReplayCameras=[bool]$VrReplayCameras
    $receipt.vrGarageMenus=[bool]$VrGarageMenus
    $receipt.performanceDisplay=($Mode -eq 'VR' -and [bool]$PerformanceDisplay)
    $receipt.performanceFollowView=[bool]$PerformanceFollowView
    $receipt.removeCinemaMatte=($Mode -eq 'VR')
    Write-UserSessionStatus $root $receipt
    if($PreviewVrFixes) {
        $receipt.message='Checking the VR renderer...'
        Write-UserSessionStatus $root $receipt
        . (Join-Path $PSScriptRoot 'Launcher-VrPreview.ps1')
        # Texture comparisons must use the same verified renderer in every mode.
        $DiagnosticProxy=Get-LauncherVrPreviewProxy -Root $root
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
    $graphics=& $python (Join-Path $PSScriptRoot 'graphics_profile.py') --output $hardware --game-root (Get-Grid2GameRoot $root) --mode $Mode --render-scale $RenderScale --desktop-vsync $DesktopVSync --desktop-msaa $DesktopMsaa --vr-msaa $VrMsaa --tree-detail $TreeDetail --object-detail $ObjectDetail --crowd-detail $CrowdDetail --texture-filtering $TextureFiltering
    if($LASTEXITCODE) { throw 'Graphics profile could not be prepared; nothing deployed.' }
    $receipt.graphics=$graphics | ConvertFrom-Json
    $graphics | Set-Content -LiteralPath (Join-Path $selection 'graphics-profile.json') -Encoding utf8
    $parameters=@{Mode=$(if($Preflight){'Preflight'}elseif($Mode -eq 'VR'){'Headset'}else{'Desktop'});XrRuntime=$XrRuntime;
        RunLogDirectory=$runLogs;RuntimeSession=$true;UntilExit=$true;BackgroundPlay='Suppress';RecenterKey=$RecenterKey;RecenterModifiers=$RecenterModifiers;
        # Request early; the native owner waits for its live renderer, camera,
        # visibility and tracking guards rather than a fixed race-start delay.
        HardwareSettingsFile=$hardware;HeadsetScale=$HeadsetScale;CaptureAfterSeconds=1;TimeoutSeconds=150;PythonPath=$python;
        FlickerDiagnostic=[bool]$FlickerDiagnostic;DiagnosticProxy=$DiagnosticProxy;
        ForceSceneResolution=($Mode -eq 'VR');VrMirrorBorderless=[bool]$VrMirrorBorderless;SkipIntro=[bool]$SkipIntro;DiagnosticLogging=[bool]$DiagnosticLogging;DrivingControls=$DrivingControls}
    if($LaunchMode -eq 'Normal') { $parameters.NormalLaunch=$true } else { $parameters.SessionDirectory=$selection }
    $parameters.HudDistance=$HudDistance; $parameters.HudFollowView=$HudFollowView; $parameters.HudHiddenElements=$HudHiddenElements
    $parameters.VrReplayCameras=$VrReplayCameras
    $parameters.VrGarageMenus=$VrGarageMenus
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
    $receipt.state='complete'; $receipt.message=if($Preflight){'Preflight passed; no game or runtime opened.'}else{'Session ended; original files restored.'}
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

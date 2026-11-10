$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
function Read-TestAst([string]$Name) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$Name"),[ref]$tokens,[ref]$errors)
    if($errors.Count) { throw "$Name has parser errors: $errors" }
    return $ast
}
function Find-TestStatement($Ast,[string]$Text) {
    $statement=$Ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.Contains($Text)},$false)
    if(!$statement) { throw "Missing routing statement: $Text" }
    return [scriptblock]::Create($statement.Extent.Text)
}
$start=Read-TestAst 'Start-UserSession.ps1'
$bind=[scriptblock]::Create($start.ParamBlock.Extent.Text+"`n@{mode=`$LaunchMode;runtime=`$XrRuntime;opponents=`$Opponents;opponentCars=`$OpponentCars;laps=`$Laps;replays=`$VrReplayCameras;garage=`$VrGarageMenus}")
$defaults=& $bind
if($defaults.mode -ne 'Practice' -or $defaults.opponents -ne 7 -or $defaults.opponentCars -cne 'same' -or $defaults.laps -ne 1 -or $defaults.replays -ne 1 -or $defaults.garage -ne 1) { throw 'Launch defaults changed.' }
foreach($mode in @('Normal','Online','Practice','Race')) {
    foreach($choice in @('same','mixed','class')) {
        $bound=& $bind -LaunchMode $mode -Opponents 1 -OpponentCars $choice -Laps 20 -VrReplayCameras 0 -VrGarageMenus 0
        if($bound.mode -ne $mode -or $bound.opponents -ne 1 -or $bound.opponentCars -cne $choice -or $bound.laps -ne 20 -or $bound.replays -or $bound.garage) { throw 'Launch options lost during binding.' }
    }
}
# Start-UserSession -> Run-StereoProbe -> Run-Diagnostic are plain scripts, not advanced
# functions: a splatted name the next script does not declare vanishes into $args. That
# dropped Online's no-preset switch in 0.2.16 (ProJict f61544b3), so every forwarded name
# must be declared by the script it reaches.
function Get-TestParameterNames($Ast) { @($Ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath }) }
$probeAst=Read-TestAst 'Run-StereoProbe.ps1'; $diagnosticAst=Read-TestAst 'Run-Diagnostic.ps1'
$probeNames=Get-TestParameterNames $probeAst; $diagnosticNames=Get-TestParameterNames $diagnosticAst
foreach($runtime in 'VDXR','SteamVR','Default') {
    if((& $bind -XrRuntime $runtime).runtime -ne $runtime) { throw 'Runtime selection lost at the launcher session entry.' }
    $probeBind=[scriptblock]::Create($probeAst.ParamBlock.Extent.Text+"`n`$XrRuntime")
    if((& $probeBind -XrRuntime $runtime -HardwareSettingsFile (Join-Path $root 'tools/stereo-probe-settings.xml')) -ne $runtime) { throw 'Runtime selection lost at Run-StereoProbe.' }
}
$forwarded=@($start.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$parameters'},$true) |
    ForEach-Object { $_.Right.Find({param($node) $node -is [Management.Automation.Language.HashtableAst]},$true).KeyValuePairs } | ForEach-Object { $_.Item1.Extent.Text })
$forwarded+=@($start.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left -is [Management.Automation.Language.MemberExpressionAst] -and
    $node.Left.Expression.Extent.Text -eq '$parameters'},$true) | ForEach-Object { $_.Left.Member.Extent.Text })
if($forwarded.Count -lt 40 -or $forwarded -notcontains 'OnlineLaunch') { throw 'Session forwarding scan found too few options.' }
$missing=@($forwarded | Where-Object { $_ -notin $probeNames } | Sort-Object -Unique)
if($missing.Count) { throw "Run-StereoProbe.ps1 silently drops session options: $($missing -join ', ')" }
$diagnosticCalls=@($probeAst.FindAll({param($node) $node -is [Management.Automation.Language.HashtableAst] -and @($node.KeyValuePairs | ForEach-Object { $_.Item1.Extent.Text }) -contains 'NormalLaunch'},$true))
if($diagnosticCalls.Count -ne 2) { throw 'Run-StereoProbe.ps1 Run-Diagnostic calls changed.' }
foreach($call in $diagnosticCalls) {
    $names=@($call.KeyValuePairs | ForEach-Object { $_.Item1.Extent.Text })
    $missing=@($names | Where-Object { $_ -notin $diagnosticNames })
    if($missing.Count -or $names -notcontains 'OnlineLaunch') { throw "Run-Diagnostic.ps1 call drops options: $($missing -join ', ') OnlineLaunch=$($names -contains 'OnlineLaunch')" }
}
$defaultFlag=@($diagnosticCalls | ForEach-Object { $_.KeyValuePairs } | Where-Object { $_.Item1.Extent.Text -eq 'DefaultOpenXrRuntime' })
if($defaultFlag.Count -ne 1) { throw 'Default OpenXR selection is not forwarded to the rendering diagnostic.' }
$desktopCall=@($diagnosticCalls | Where-Object { @($_.KeyValuePairs | ForEach-Object { $_.Item1.Extent.Text }) -contains 'PlayerControlOnly' })
if($desktopCall.Count -ne 1) { throw 'Missing Desktop diagnostic call.' }
foreach($name in @('CockpitMode','DesktopSeatPosition','DesktopCockpitFov')) {
    $entry=@($desktopCall[0].KeyValuePairs | Where-Object { $_.Item1.Extent.Text -eq $name })
    if($entry.Count -ne 1 -or $entry[0].Item2.Extent.Text -ne ('$'+$name) -or $name -notin $forwarded) { throw "Desktop camera option lost in session chain: $name" }
}
$seatHandoffNode=$probeAst.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$startInCockpit'},$true)
if(!$seatHandoffNode) { throw 'Missing cockpit startup-view decision.' }
$seatHandoff=[scriptblock]::Create($seatHandoffNode.Extent.Text)
foreach($case in @(@('Preflight',$true,$false),@('Desktop',$true,$false),@('Preflight',$false,$true),@('Headset',$false,$true))) {
    & { $CockpitAiDiagnostic=$false;$UntilExit=$true;$Mode=$case[0];$PlayerControlOnly=$case[1];$selectedSession=@{control='human'}
        . $seatHandoff
        if($startInCockpit -ne $case[2]) { throw 'Desktop forced a VR startup camera, or VR lost its startup camera.' }
    }
}
$defaultExpression=[scriptblock]::Create($defaultFlag[0].Item2.Extent.Text)
foreach($case in @(@('Headset','Default',$true),@('Desktop','Default',$false),@('Headset','VDXR',$false),@('Headset','SteamVR',$false))) {
    & { $Mode=$case[0];$XrRuntime=$case[1]
        if([bool](& $defaultExpression) -ne $case[2]) { throw 'Default OpenXR must reach VR only, without changing explicit providers.' }
    }
}
# Run-StereoProbe re-checks the scene size it was given against the headset plan, which must
# include the field of view: 0.2.16 refused every VR launch below 100%.
. (Join-Path $root 'tools/Headset-Resolution.ps1')
$planStatement=$probeAst.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$plan' -and $node.Extent.Text.Contains('Get-HeadsetResolutionPlan')},$true)
$sceneCheck=@($probeAst.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.Contains('Headset and scene resolution requests do not match')},$true) |
    Sort-Object { $_.Extent.Text.Length } | Select-Object -First 1)[0]
if(!$planStatement -or !$sceneCheck) { throw 'Run-StereoProbe.ps1 headset plan check moved.' }
foreach($case in @(@{fov=100;crop='All'},@{fov=80;crop='All'},@{fov=55;crop='Vertical'})) {
    $expected=Get-HeadsetResolutionPlan 3072 3264 150 $case.fov $case.crop
    & { $HeadsetWidth=3072;$HeadsetHeight=3264;$VrRenderScale=150;$VrFieldOfView=$case.fov;$VrFieldOfViewCrop=$case.crop
        $ForceSceneResolution=$true;$Mode='Headset';$sceneWidth=$expected.sceneWidth;$sceneHeight=$expected.sceneHeight
        . ([scriptblock]::Create($planStatement.Extent.Text)); . ([scriptblock]::Create($sceneCheck.Extent.Text)) }
}
foreach($invalid in @(@{LaunchMode='Replay'},@{Opponents=0},@{Opponents=8},@{OpponentCars='unknown'},@{Laps=21},@{VrReplayCameras=2},@{VrGarageMenus=2})) {
    $rejected=$false
    try { $null=& $bind @invalid } catch [Management.Automation.ParameterBindingException] { $rejected=$true }
    if(!$rejected) { throw 'Invalid launch option accepted.' }
}

# Experimental native left eye: an off-by-default switch, forwarded as Replace for VR only.
$nativeBind=[scriptblock]::Create($start.ParamBlock.Extent.Text+"`n[bool]`$NativeLeftEye")
if((& $nativeBind) -or !(& $nativeBind -NativeLeftEye)) { throw 'Native left-eye switch binding changed.' }
$nativeForward=$start.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$parameters.NativeLeftEye'},$true)
if(!$nativeForward) { throw 'Native left-eye switch is not forwarded.' }
foreach($case in @(@('VR',$true,'Replace'),@('VR',$false,'Off'),@('Desktop',$true,'Off'))) {
    $Mode=$case[0];$NativeLeftEye=$case[1];$parameters=@{}
    . ([scriptblock]::Create($nativeForward.Extent.Text))
    if($parameters.NativeLeftEye -cne $case[2]) { throw "Native left-eye forwarding wrong for $($case[0])/$($case[1])." }
}
foreach($script in @('Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
    $attribute=(Read-TestAst $script).ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq 'NativeLeftEye' }
    if(!$attribute -or $attribute.Extent.Text -notmatch "ValidateSet\('Off','Compare','Replace'\)") { throw "$script lacks the native left-eye modes." }
}

# Board bias is opt-in and ignored for Desktop at the user-session boundary.
$sponsorBind=[scriptblock]::Create($start.ParamBlock.Extent.Text+"`n[bool]`$SponsorDepthBias")
if((& $sponsorBind) -or !(& $sponsorBind -SponsorDepthBias)) { throw 'Sponsor bias switch default changed.' }
$sponsorForward=$start.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$parameters.SponsorDepthBias'},$true)
if(!$sponsorForward) { throw 'Sponsor bias is not forwarded.' }
foreach($case in @(@('VR',$true,$true),@('VR',$false,$false),@('Desktop',$true,$false))) {
    $Mode=$case[0];$SponsorDepthBias=$case[1];$parameters=@{}
    . ([scriptblock]::Create($sponsorForward.Extent.Text))
    if($parameters.SponsorDepthBias -ne $case[2]) { throw 'Sponsor bias must apply only to opted-in VR sessions.' }
}
$sponsorFlag=@($diagnosticCalls | ForEach-Object { $_.KeyValuePairs } | Where-Object { $_.Item1.Extent.Text -eq 'SponsorDepthBias' })
if($sponsorFlag.Count -ne 1) { throw 'Sponsor bias is not forwarded to the rendering diagnostic.' }

# Execute the actual preparation/routing branches in an owned fixture. External
# Python is replaced with a recorder; no game, Steam or hardware files are read.
$fixture=Join-Path $root ('build/launch-mode-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
try {
    $writeSponsor=Find-TestStatement $diagnosticAst "'sponsor-depth-bias.request'"
    $run=$fixture;$SponsorDepthBias=$false;. $writeSponsor
    $sponsorRequest=Join-Path $fixture 'sponsor-depth-bias.request'
    if(Test-Path -LiteralPath $sponsorRequest) { throw 'Disabled sponsor bias wrote a request.' }
    $SponsorDepthBias=$true;. $writeSponsor
    if(!(Test-Path -LiteralPath $sponsorRequest) -or (Get-Item -LiteralPath $sponsorRequest).Length -ne 0) { throw 'Enabled sponsor bias did not select native defaults.' }
    Remove-Item -LiteralPath $sponsorRequest
    $writeDefault=Find-TestStatement $diagnosticAst "'xr-default-runtime.request'"
    & { $run=$fixture;$DefaultOpenXrRuntime=$false;. $writeDefault }
    $defaultRequest=Join-Path $fixture 'xr-default-runtime.request'
    if(Test-Path -LiteralPath $defaultRequest) { throw 'An explicit provider gained the Default runtime request.' }
    & { $run=$fixture;$DefaultOpenXrRuntime=$true;. $writeDefault }
    if([IO.File]::ReadAllText($defaultRequest) -cne 'Default') { throw 'Renderer Default runtime request was not written.' }
    Remove-Item -LiteralPath $defaultRequest
    # Exercise the launch progress transport with fixture receipts and fake owned
    # processes. No live window or game is inspected or controlled.
    $progressStatements=@($start.FindAll({param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
        ($node.Clauses[0].Item1.Extent.Text.Contains("StartsWith('GRID2VR_LAUNCH|'") -or
         $node.Clauses[0].Item1.Extent.Text.Contains("-ceq 'GRID2VR_GAME_WINDOW_READY'"))
    },$true))
    if($progressStatements.Count -ne 2) { throw 'Launch progress receiver missing.' }
    $receive=[scriptblock]::Create(($progressStatements.Extent.Text -join "`n"))
    & {
        param($Receiver,$Directory)
        function Get-Grid2DataRoot { return $Directory }
        function Write-SessionJson($Path,$Value) { [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json)) }
        function Write-UserSessionStatus($Root,$Value) { Write-SessionJson (Join-Path $Directory 'user-session-last.json') $Value }
        foreach($Mode in @('Desktop','VR')) {
            $receipt=@{state='starting'}
            $_='GRID2VR_LAUNCH|Backing up original files...'; . $Receiver
            $saved=Get-Content -LiteralPath (Join-Path $Directory 'user-session-last.json') -Raw | ConvertFrom-Json
            if($saved.message -ne 'Backing up original files...' -or $saved.state -ne 'starting' -or $saved.gameWindowReady) { throw 'Progress lost or changed session state.' }
            $_='unrelated worker output'; . $Receiver
            if($receipt.message -ne $saved.message) { throw 'Ordinary output changed progress.' }
            $_='GRID2VR_GAME_WINDOW_READY'; . $Receiver
            if(!$receipt.gameWindowReady -or $receipt.state -ne 'starting') { throw 'Game window observation was not recorded.' }
        }
    } $receive $fixture
    $diagnostic=Read-TestAst 'Run-Diagnostic.ps1'
    $windowProgress=Find-TestStatement $diagnostic 'if($UntilExit -and !$gameWindowReported)'
    foreach($case in @('no-process','hidden','visible','exited','read-failed','already-reported','bounded')) {
        $UntilExit=$case -ne 'bounded'; $gameWindowReported=$case -eq 'already-reported'; $handles=@{}
        $owned=[pscustomobject]@{HasExited=($case -eq 'exited');MainWindowHandle=[IntPtr]$(if($case -eq 'hidden'){0}else{1})}
        $owned | Add-Member ScriptMethod Refresh $(if($case -eq 'read-failed'){{throw 'Fixture process exited.'}}else{{}})
        if($case -ne 'no-process') { $handles[123]=$owned }
        $output=@(. $windowProgress)
        if(($output -contains 'GRID2VR_GAME_WINDOW_READY') -ne ($case -eq 'visible')) { throw "Incorrect game-window progress for $case." }
    }
    # Execute the real post-recovery decisions without launching or deploying.
    # Missing activation is only a note for a clean Normal headset session.
    $completion=Find-TestStatement (Read-TestAst 'Run-StereoProbe.ps1') 'Background play guards did not pass'
    function Assert-CareerProfileSession {} # Profile routing has separate tests.
    $run=$fixture; $RuntimeSession=$true; $AutoCaptureFlicker=$false
    foreach($case in @('unobserved','practice','desktop','bounded','input-probe','observe-probe','renderer-failed',
                       'missing-stop','crash','fault','background-failed','background-observed','background-healthy',
                       'no-classification','no-readiness','active-background')) {
        $NormalLaunch=$true; $UntilExit=$true; $Mode='Headset'; $PlayerControlOnly=$false
        $PlayerControlProbe='Off'; $BackgroundPlay='Suppress'
        $data=@{normalUserProcessExit=$true;rendererHealthy=$true;nativeHealthy=$false;lastRenderingHealthy=$true;
            status=@{failed=$false};unexpectedFaults=@();readiness=@{backgroundPlay=$false};
            backgroundPlayVerification='not_observed';backgroundPlayObservedHealthy=$false;backgroundPlayProbeHealthy=$false}
        switch($case) {
            'practice' {$NormalLaunch=$false}
            'desktop' {$Mode='Desktop';$PlayerControlOnly=$true}
            'bounded' {$UntilExit=$false}
            'input-probe' {$PlayerControlProbe='Observe'}
            'observe-probe' {$BackgroundPlay='Observe'}
            'renderer-failed' {$data.status.failed=$true;$data.rendererHealthy=$false;$data.lastRenderingHealthy=$false}
            'missing-stop' {$data.rendererHealthy=$false}
            'crash' {$data.normalUserProcessExit=$false}
            'fault' {$data.unexpectedFaults=@('unexpected fault');$data.rendererHealthy=$false}
            'background-failed' {$data.backgroundPlayVerification='failed'}
            'background-observed' {$data.backgroundPlayVerification='observed';$data.backgroundPlayObservedHealthy=$true}
            'background-healthy' {$data.backgroundPlayVerification='healthy';$data.backgroundPlayObservedHealthy=$true;$data.backgroundPlayProbeHealthy=$true;$data.nativeHealthy=$true}
            'no-classification' {$data.Remove('backgroundPlayVerification')}
            'no-readiness' {$data.Remove('readiness')}
            'active-background' {$data.readiness.backgroundPlay=$true}
        }
        $data | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'runtime-summary.json')
        $failure=$null; $message=$null
        try { $message=& $completion } catch { $failure=$_.Exception.Message }
        if($case -in @('unobserved','background-observed','background-healthy')) {
            if($failure) { throw "Clean completion rejected ($case): $failure" }
            if(($message -like '*Background play was not verified*') -ne ($case -eq 'unobserved')) { throw "Wrong completion message: $case" }
        } elseif(!$failure) { throw "Unsafe completion accepted: $case" }
        if($case -eq 'renderer-failed' -and $failure -notlike '*rendering guard*') { throw 'Primary renderer failure was hidden.' }
        if($case -eq 'fault' -and $failure -notlike '*Game exception*') { throw 'Primary game fault was hidden.' }
    }
    # The final launcher receipt shows the note, never replaces a failure, and
    # remains compatible with summaries produced before classification existed.
    . (Join-Path $root 'tools/User-Session.ps1')
    $annotate=$start.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -eq '$receipt.run'},$true)
    if(!$annotate) { throw 'Session evidence annotation missing.' }
    $annotate=[scriptblock]::Create($annotate.Extent.Text)
    foreach($verification in @('not_observed','failed','healthy',$null)) {
        @{backgroundPlayVerification=$verification} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'runtime-summary.json')
        foreach($state in @('complete','failed')) {
            $receipt=@{run=$run;state=$state;message='Original message.'}
            . $annotate
            $expected='Original message.'
            if($state -eq 'complete' -and $verification -eq 'not_observed') { $expected+=' Background play was not verified (no activation receipt).' }
            if($receipt.message -cne $expected -or $receipt.backgroundPlayVerification -ne $verification) { throw 'Launcher completion note lost or hid a failure.' }
        }
    }

    $prepare=Find-TestStatement $start '$selectionArguments=@('
    $prepare=[scriptblock]::Create('param([string]$PSScriptRoot)'+[Environment]::NewLine+$prepare.ToString())
    $route=Find-TestStatement $start '$parameters.NormalLaunch=$true'
    $menus=$start.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$nativeMenus'},$true)
    if(!$menus) { throw 'Missing native-menu launch mode statement.' }
    $menus=[scriptblock]::Create($menus.Extent.Text)
    function Invoke-ModeTestPython {
        $script:pythonArguments=@($args)
        $global:LASTEXITCODE=0
        return '{"carContent":{"dlc":false,"cockpitAvailable":true}}'
    }
    $python='Invoke-ModeTestPython'; $Car='z06'; $Track='cities/chicago/route_0'; $sessionLaps=20; $Opponents=7
    foreach($OpponentCars in @('same','mixed','class')) {
    foreach($LaunchMode in @('Normal','Online','Practice','Race')) {
        $selection=Join-Path $fixture ($LaunchMode+'-'+$OpponentCars)
        $script:pythonArguments=@(); $parameters=@{}
        . $menus
        . $prepare (Join-Path $root 'tools')
        . $route
        if($LaunchMode -in @('Normal','Online')) {
            if($script:pythonArguments.Count -or !(Test-Path -LiteralPath $selection -PathType Container) -or
               !$parameters.NormalLaunch -or $parameters.ContainsKey('SessionDirectory') -or
               @(Get-ChildItem -LiteralPath $selection -Force).Count) { throw 'Normal launch prepared or routed a selected session.' }
            if($parameters.OnlineLaunch -ne ($LaunchMode -eq 'Online')) { throw 'Online launch lost its no-preset route.' }
        } else {
            if($parameters.NormalLaunch -or $parameters.OnlineLaunch -or $parameters.SessionDirectory -ne $selection) { throw 'Selected session was routed to Normal.' }
            $captured=$script:pythonArguments
            $modeIndex=[Array]::IndexOf($captured,'--mode'); $opponentIndex=[Array]::IndexOf($captured,'--opponents')
            if($modeIndex -lt 0 -or $captured[$modeIndex+1] -cne $LaunchMode.ToLowerInvariant() -or
               $captured -notcontains '--practice-bundle' -or $captured -notcontains 'human') { throw 'Selected mode lost guarded human entry.' }
            $bundleIndex=[Array]::IndexOf($captured,'--practice-bundle')
            $expectedBank=if($LaunchMode -eq 'Practice'){'practice-menus-20261106'}else{'practice-flow-20261014'}
            if((Split-Path $captured[$bundleIndex+1] -Leaf) -cne $expectedBank) { throw 'Post-Practice menus escaped Practice routing.' }
            if($LaunchMode -eq 'Race' -and ($opponentIndex -lt 0 -or $captured[$opponentIndex+1] -ne 7)) { throw 'Race opponent count was lost.' }
            $carsIndex=[Array]::IndexOf($captured,'--opponent-cars')
            if($LaunchMode -eq 'Race' -and ($carsIndex -lt 0 -or $captured[$carsIndex+1] -cne $OpponentCars)) { throw 'Race opponent car choice was lost.' }
            if($LaunchMode -eq 'Practice' -and $carsIndex -ge 0) { throw 'Practice acquired a race opponent choice.' }
            if($LaunchMode -eq 'Practice' -and $opponentIndex -ge 0) { throw 'Practice gained AI opponents.' }
        }
    }
    }

    foreach($name in @('Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        $ast=Read-TestAst $name
        $normalGuard=Find-TestStatement $ast 'Normal launch requires the guarded runtime'
        $sessionGuard=Find-TestStatement $ast $(if($name -eq 'Run-StereoProbe.ps1'){'User sessions require selected human practice/race'}else{'User-run sessions require guarded human practice/race'})
        $Mode='Headset'; $Action='Run'; $RunKind='Probe'; $PlayerControlProbe='Off'; $UntilExit=$true; $RuntimeSession=$true
        foreach($sessionMode in @('normal','practice','race')) {
            $NormalLaunch=$sessionMode -eq 'normal'
            $SessionDirectory=if($NormalLaunch){''}else{'owned-selection'}
            $selectedSession=if($NormalLaunch){$null}else{@{control='human';selection=@{mode=$sessionMode}}}
            & $normalGuard; & $sessionGuard
        }
        foreach($case in @('implicit','selected-normal','no-runtime','input-probe','ai-user-session')) {
            $RuntimeSession=$true; $NormalLaunch=$true; $SessionDirectory=''; $PlayerControlProbe='Off'; $selectedSession=$null
            switch($case) {
                'implicit' {$NormalLaunch=$false}
                'selected-normal' {$SessionDirectory='owned-selection';$selectedSession=@{control='human'}}
                'no-runtime' {$RuntimeSession=$false}
                'input-probe' {$PlayerControlProbe='Human'}
                'ai-user-session' {$NormalLaunch=$false;$selectedSession=@{control='ai'}}
            }
            $rejected=$false
            try { & $normalGuard; & $sessionGuard } catch { $rejected=$true }
            if(!$rejected) { throw "$name accepted invalid mode combination: $case" }
        }
    }

    $diagnostic=Read-TestAst 'Run-Diagnostic.ps1'
    # Execute fallback and exposure routing using verified content metadata.
    # No files are copied into a game and no process is launched.
    $workerFallback=$start.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -eq '$content.dlc -and !$content.cockpitAvailable'},$true)
    if(!$workerFallback) { throw 'DLC worker camera fallback missing.' }
    $workerFallback=[scriptblock]::Create($workerFallback.Extent.Text)
    foreach($item in @(@{dlc=$false;cockpitAvailable=$true},@{dlc=$true;cockpitAvailable=$false})) {
        $content=$item; $CockpitMode='HeadOnly'; $receipt=@{}
        . $workerFallback
        if(($CockpitMode -eq 'Off') -ne $content.dlc -or [bool]$receipt.cameraNote -ne $content.dlc) { throw 'DLC worker fallback differs.' }
    }
    foreach($name in @('Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        $ast=Read-TestAst $name
        $fallback=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Clauses[0].Item1.Extent.Text -eq '$selectedSession.carContent.dlc -and !$selectedSession.carContent.cockpitAvailable'},$true)
        if(!$fallback) { throw "DLC fallback missing: $name" }
        $fallback=[scriptblock]::Create($fallback.Extent.Text)
        foreach($item in @($null,@{dlc=$false;cockpitAvailable=$true},@{dlc=$true;cockpitAvailable=$false})) {
            foreach($requested in @('Off','HeadOnly','HideDriver')) {
                $selectedSession=if($item){@{carContent=$item}}else{$null}
                $CockpitMode=$requested
                . $fallback
                $expected=if($item.dlc){'Off'}else{$requested}
                if($CockpitMode -ne $expected) { throw "DLC fallback changed a supported or Normal camera: $name" }
            }
        }
    }
    $run=Join-Path $fixture 'dlc-camera'; [IO.Directory]::CreateDirectory($run) | Out-Null
    $CockpitMode='Off'; $NativeCockpitExposure=$false
    $exposure=Find-TestStatement $diagnostic "'cockpit-exposure.request'"
    . $exposure
    if(Test-Path -LiteralPath (Join-Path $run 'cockpit-exposure.request')) { throw 'Original DLC camera received a cockpit exposure override.' }
    # The VR impact-glitch removal is default on, follows the opt-out, and never reaches desktop runs.
    $impact=Find-TestStatement $diagnostic "'impact-interference.request'"
    $impactRequest=Join-Path $run 'impact-interference.request'
    foreach($case in @(@{stereo=$true;native=$false;written=$true},@{stereo=$true;native=$true;written=$false},@{stereo=$false;native=$false;written=$false})) {
        & { $RuntimeSession=$true;$ReplayStereo=$case.stereo;$PlayerControlOnly=$false;$NativeImpactEffect=$case.native; . $impact }
        if((Test-Path -LiteralPath $impactRequest) -ne $case.written) { throw "Impact glitch request routing changed: stereo=$($case.stereo) native=$($case.native)" }
        if(Test-Path -LiteralPath $impactRequest) { Remove-Item -LiteralPath $impactRequest }
    }
    # The VR garage floor reflection removal is default on and never reaches desktop runs.
    $floor=Find-TestStatement $diagnostic "'floor-reflection.request'"
    $floorRequest=Join-Path $run 'floor-reflection.request'
    foreach($case in @(@{stereo=$true;player=$false;written=$true},@{stereo=$false;player=$false;written=$false},@{stereo=$true;player=$true;written=$false})) {
        & { $RuntimeSession=$true;$ReplayStereo=$case.stereo;$PlayerControlOnly=$case.player; . $floor }
        if((Test-Path -LiteralPath $floorRequest) -ne $case.written) { throw "Floor reflection request routing changed: stereo=$($case.stereo) desktop=$($case.player)" }
        if(Test-Path -LiteralPath $floorRequest) { Remove-Item -LiteralPath $floorRequest }
    }
    # The VR intro near plane is default on, never reaches Desktop, non-stereo or Online runs.
    $introStatement=Find-TestStatement $diagnostic "'intro-near-plane.request'"
    $introRequest=Join-Path $run 'intro-near-plane.request'
    foreach($case in @(@{stereo=$true;player=$false;online=$false;written=$true},@{stereo=$true;player=$false;online=$true;written=$false},
        @{stereo=$false;player=$false;online=$false;written=$false},@{stereo=$true;player=$true;online=$false;written=$false})) {
        & { $RuntimeSession=$true;$ReplayStereo=$case.stereo;$PlayerControlOnly=$case.player;$OnlineLaunch=$case.online; . $introStatement }
        if((Test-Path -LiteralPath $introRequest) -ne $case.written) { throw "Intro near plane routing changed: stereo=$($case.stereo) desktop=$($case.player) online=$($case.online)" }
        if(Test-Path -LiteralPath $introRequest) { Remove-Item -LiteralPath $introRequest }
    }
    . (Join-Path $root 'tools/Launch-Backend.ps1')
    function Get-Grid2SteamRoot { return 'C:\Fixture Steam' }
    $plan=Get-Grid2LaunchPlan -Game 'C:\Fixture Game' -Backend Steam -NormalLaunch
    if(($plan.arguments -join ' ') -cne '-applaunch 44350') { throw 'Normal launch acquired a benchmark route.' }
    $plan=Get-Grid2LaunchPlan -Game 'C:\Fixture Game' -Backend Steam
    if(($plan.arguments -join ' ') -cne '-applaunch 44350 -benchmark example_benchmark.xml') { throw 'Legacy diagnostic route changed.' }

    # The real deployment gate must not even prepare a flow backup in Normal.
    $flow=Find-TestStatement $diagnostic '$practiceFlow=New-PracticeFlowDeployment'
    function New-PracticeFlowDeployment { $script:flowCalls++; return @{fixture=$true} }
    $script:flowCalls=0; $selectedSession=$null; $game='unused'; $run=$fixture
    . $flow
    if($script:flowCalls) { throw 'Normal launch attempted flow deployment.' }
    foreach($sessionMode in @('practice','race')) {
        $selectedSession=@{control='human';selection=@{mode=$sessionMode}}
        . $flow
    }
    if($script:flowCalls -ne 2) { throw 'Selected human mode lost its flow transaction.' }

    # Normal and Online launches never deploy the session wheel preset: GRID 2's online
    # race-start check rejects it (ProJict 1d47bf5c), and GRID 2 stops autosaving the
    # career while input/paths.xml is modified (ProJict 9214d7e0, GitHub #4). Only
    # selected Practice/Race sessions, which never save the career, may deploy it.
    $onlineGuard=Find-TestStatement $diagnostic 'Online launch uses the native menus'
    $preset=Find-TestStatement $diagnostic 'New-DevicePresetDeployment'
    function New-DevicePresetDeployment { $script:presetCalls++; return @{fixture=$true} }
    foreach($case in @(@{normal=$true;online=$false;claim=$true;preset=0},@{normal=$true;online=$true;claim=$true;preset=0},
                       @{normal=$false;online=$false;claim=$true;preset=1},@{normal=$true;online=$false;claim=$false;preset=0})) {
        $NormalLaunch=$case.normal; $OnlineLaunch=$case.online; $deviceClaimWritten=$case.claim; $script:presetCalls=0
        & $onlineGuard
        $result=& $preset
        if($script:presetCalls -ne $case.preset -or [bool]$result -ne [bool]$case.preset) { throw "Device preset routing changed: normal=$($case.normal) online=$($case.online)" }
    }
    $NormalLaunch=$false; $OnlineLaunch=$true; $rejected=$false
    try { & $onlineGuard } catch { $rejected=$true }
    if(!$rejected) { throw 'Online launch accepted a selected session.' }

    $normalRequest=Find-TestStatement $diagnostic "'normal-launch.request'"
    $seatNode=$diagnostic.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.Contains('$seatSession=New-SeatSessionRequest')},$false)
    if(!$seatNode) { throw 'Missing in-game seat session preparation.' }
    $seatGuard=[scriptblock]::Create($seatNode.Clauses[0].Item1.Extent.Text)
    foreach($online in @($false,$true)) {
        foreach($runtime in @($false,$true)) {
            foreach($until in @($false,$true)) {
                $OnlineLaunch=$online;$RuntimeSession=$runtime;$UntilExit=$until;$CockpitMode='HeadOnly'
                if([bool](& $seatGuard) -ne (!$online -and $runtime -and $until)) { throw 'Seat tool leaked into Online or a bounded/non-runtime run.' }
            }
        }
    }
    $RuntimeSession=$true;$UntilExit=$true;$OnlineLaunch=$false;$CockpitMode='Off'
    if(& $seatGuard) { throw 'Original-camera mode armed the seat tool.' }
    # Find the inner Normal-mode statement, without invoking deployment/helpers.
    $inner=$seatNode.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.StartsWith('if($inGameSeatRequested -and $NormalLaunch)')},$true)
    if(!$inner) { throw 'Normal seat mode has no separate marker gate.' }
    $nativeSeat=[scriptblock]::Create($inner.Extent.Text)
    $run=Join-Path $fixture 'seat-native-entry';[IO.Directory]::CreateDirectory($run) | Out-Null
    $proxy=Join-Path $run 'fixture.dll';[IO.File]::WriteAllText($proxy,'grid2vr-seat-menu-v1 grid2vr-seat-native-menus-v1')
    $marker=Join-Path $run 'seat-native-menus.request'
    $inGameSeatRequested=$true;$NormalLaunch=$false;. $nativeSeat
    if([IO.File]::Exists($marker)) { throw 'Direct launch received a native-menu seat override.' }
    $NormalLaunch=$true;. $nativeSeat
    if([IO.File]::ReadAllText($marker) -cne "grid2vr-seat-native-menus-v1`n") { throw 'Normal seat protocol changed.' }
    [IO.File]::Delete($marker);[IO.File]::WriteAllText($proxy,'grid2vr-seat-menu-v1')
    $rejected=$false;try{. $nativeSeat}catch{$rejected=$true}
    if(!$rejected -or [IO.File]::Exists($marker)) { throw 'Normal seat mode accepted an old renderer.' }
    . (Join-Path $PSScriptRoot '../tools/Dlc-Cockpit.ps1')
    [IO.File]::WriteAllText($proxy,'dlc-cockpit-v1 dlc-cockpit-desktop-v1')
    Assert-DlcCockpitProxy $proxy
    $rejected=$false;try { Assert-DlcCockpitProxy $proxy $false $true } catch { $rejected=$true }
    if(!$rejected) { throw 'Offline Normal accepted a renderer missing native-menu DLC cockpit support.' }
    [IO.File]::AppendAllText($proxy,' grid2vr-offline-normal-v1')
    $rejected=$false;try { Assert-DlcCockpitProxy $proxy $false $true } catch { $rejected=$true }
    if(!$rejected) { throw 'Offline Normal accepted a renderer without embedded base-car support.' }
    [IO.File]::AppendAllText($proxy,' grid2vr-offline-normal-v2')
    Assert-DlcCockpitProxy $proxy $false $true
    Assert-DlcCockpitProxy $proxy $true $true
    $raceRequest=Find-TestStatement $diagnostic "'race-launch.request'"
    $replayRequest=Find-TestStatement $diagnostic "'flat-replays.request'"
    $garageRequest=Find-TestStatement $diagnostic "'flat-garage.request'"
    foreach($sessionMode in @('normal','online','practice','race')) {
        $run=Join-Path $fixture ('requests-'+$sessionMode)
        [IO.Directory]::CreateDirectory($run) | Out-Null
        $OnlineLaunch=$sessionMode -eq 'online'; $NormalLaunch=$sessionMode -in @('normal','online')
        $selectedSession=if($NormalLaunch){$null}else{@{selection=@{mode=$sessionMode}}}
        . $normalRequest; . $raceRequest
        if([IO.File]::Exists((Join-Path $run 'normal-launch.request')) -ne $NormalLaunch -or
           [IO.File]::Exists((Join-Path $run 'race-launch.request')) -ne ($sessionMode -eq 'race')) { throw 'Native launch-mode marker differs.' }
        if($NormalLaunch) {
            $offline=[IO.File]::ReadAllText((Join-Path $run 'normal-launch.request')) -ceq "grid2vr-offline-normal-v2`n"
            if($offline -ne (!$OnlineLaunch)) { throw "Wrong offline cockpit permission for $sessionMode." }
        }
    }
    $RuntimeSession=$true; $ReplayStereo=$true; $PlayerControlOnly=$false; $VrReplayCameras=1
    . $replayRequest
    $flat=Join-Path $run 'flat-replays.request'
    if([IO.File]::Exists($flat)) { throw 'Default replay setting changed.' }
    $VrReplayCameras=0; . $replayRequest
    if(![IO.File]::Exists($flat)) { throw 'Flat replay request missing.' }
    [IO.File]::Delete($flat); $PlayerControlOnly=$true; . $replayRequest
    if([IO.File]::Exists($flat)) { throw 'Desktop-only session received a VR replay override.' }
    $PlayerControlOnly=$false; $VrGarageMenus=1
    . $garageRequest
    $flat=Join-Path $run 'flat-garage.request'
    if([IO.File]::Exists($flat)) { throw 'Default garage setting changed.' }
    $VrGarageMenus=0; . $garageRequest
    if(![IO.File]::Exists($flat)) { throw 'Flat garage request missing.' }
    [IO.File]::Delete($flat); $PlayerControlOnly=$true; . $garageRequest
    if([IO.File]::Exists($flat)) { throw 'Desktop-only session received a VR garage override.' }
} finally {
    $absolute=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($absolute) -ne (Join-Path $root 'build')) { throw 'Mode fixture escaped build.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
'Launch modes, limits, native/selected routing, reversible flow isolation and replay request defaults passed.'

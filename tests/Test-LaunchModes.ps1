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
$bind=[scriptblock]::Create($start.ParamBlock.Extent.Text+"`n@{mode=`$LaunchMode;opponents=`$Opponents;opponentCars=`$OpponentCars;laps=`$Laps;replays=`$VrReplayCameras;garage=`$VrGarageMenus}")
$defaults=& $bind
if($defaults.mode -ne 'Practice' -or $defaults.opponents -ne 7 -or $defaults.opponentCars -cne 'same' -or $defaults.laps -ne 1 -or $defaults.replays -ne 1 -or $defaults.garage -ne 1) { throw 'Launch defaults changed.' }
foreach($mode in @('Normal','Practice','Race')) {
    foreach($choice in @('same','mixed','class')) {
        $bound=& $bind -LaunchMode $mode -Opponents 1 -OpponentCars $choice -Laps 20 -VrReplayCameras 0 -VrGarageMenus 0
        if($bound.mode -ne $mode -or $bound.opponents -ne 1 -or $bound.opponentCars -cne $choice -or $bound.laps -ne 20 -or $bound.replays -or $bound.garage) { throw 'Launch options lost during binding.' }
    }
}
foreach($invalid in @(@{LaunchMode='Replay'},@{Opponents=0},@{Opponents=8},@{OpponentCars='unknown'},@{Laps=21},@{VrReplayCameras=2},@{VrGarageMenus=2})) {
    $rejected=$false
    try { $null=& $bind @invalid } catch [Management.Automation.ParameterBindingException] { $rejected=$true }
    if(!$rejected) { throw 'Invalid launch option accepted.' }
}

# Execute the actual preparation/routing branches in an owned fixture. External
# Python is replaced with a recorder; no game, Steam or hardware files are read.
$fixture=Join-Path $root ('build/launch-mode-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
try {
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
    function Invoke-ModeTestPython {
        $script:pythonArguments=@($args)
        $global:LASTEXITCODE=0
        return '{"carContent":{"dlc":false,"cockpitAvailable":true}}'
    }
    $python='Invoke-ModeTestPython'; $Car='z06'; $Track='cities/chicago/route_0'; $sessionLaps=20; $Opponents=7
    foreach($OpponentCars in @('same','mixed','class')) {
    foreach($LaunchMode in @('Normal','Practice','Race')) {
        $selection=Join-Path $fixture ($LaunchMode+'-'+$OpponentCars)
        $script:pythonArguments=@(); $parameters=@{}
        . $prepare (Join-Path $root 'tools')
        . $route
        if($LaunchMode -eq 'Normal') {
            if($script:pythonArguments.Count -or !(Test-Path -LiteralPath $selection -PathType Container) -or
               !$parameters.NormalLaunch -or $parameters.ContainsKey('SessionDirectory') -or
               @(Get-ChildItem -LiteralPath $selection -Force).Count) { throw 'Normal launch prepared or routed a selected session.' }
        } else {
            if($parameters.NormalLaunch -or $parameters.SessionDirectory -ne $selection) { throw 'Selected session was routed to Normal.' }
            $captured=$script:pythonArguments
            $modeIndex=[Array]::IndexOf($captured,'--mode'); $opponentIndex=[Array]::IndexOf($captured,'--opponents')
            if($modeIndex -lt 0 -or $captured[$modeIndex+1] -cne $LaunchMode.ToLowerInvariant() -or
               $captured -notcontains '--practice-bundle' -or $captured -notcontains 'human') { throw 'Selected mode lost guarded human entry.' }
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
    $steam=$diagnostic.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$steamArgumentLine'},$false)
    if(!$steam) { throw 'Steam route assignment missing.' }
    $steamRoute=[scriptblock]::Create($steam.Extent.Text)
    $NormalLaunch=$true; . $steamRoute
    if($steamArgumentLine -cne '-applaunch 44350') { throw 'Normal launch acquired a benchmark route.' }
    $NormalLaunch=$false; . $steamRoute
    if($steamArgumentLine -cne '-applaunch 44350 -benchmark example_benchmark.xml') { throw 'Legacy diagnostic route changed.' }

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

    $normalRequest=Find-TestStatement $diagnostic "'normal-launch.request'"
    $raceRequest=Find-TestStatement $diagnostic "'race-launch.request'"
    $replayRequest=Find-TestStatement $diagnostic "'flat-replays.request'"
    $garageRequest=Find-TestStatement $diagnostic "'flat-garage.request'"
    foreach($sessionMode in @('normal','practice','race')) {
        $run=Join-Path $fixture ('requests-'+$sessionMode)
        [IO.Directory]::CreateDirectory($run) | Out-Null
        $NormalLaunch=$sessionMode -eq 'normal'; $selectedSession=if($NormalLaunch){$null}else{@{selection=@{mode=$sessionMode}}}
        . $normalRequest; . $raceRequest
        if([IO.File]::Exists((Join-Path $run 'normal-launch.request')) -ne $NormalLaunch -or
           [IO.File]::Exists((Join-Path $run 'race-launch.request')) -ne ($sessionMode -eq 'race')) { throw 'Native launch-mode marker differs.' }
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

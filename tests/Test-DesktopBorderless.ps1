$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Borderless-Desktop.ps1')
$process=[Diagnostics.Process]::GetCurrentProcess()
try {
    $run=Join-Path $root 'build/desktop-window-test'
    $command=New-DesktopBorderlessCommand $root $run $process
    if($command.UseShellExecute -or !$command.CreateNoWindow -or $command.WorkingDirectory -ne $root -or
       $command.ArgumentList.Count -ne 4 -or $command.ArgumentList[0] -cne '--borderless-desktop' -or
       $command.ArgumentList[1] -cne $run -or $command.ArgumentList[2] -cne [string]$process.Id -or
       $command.ArgumentList[3] -cne [string]$process.StartTime.ToUniversalTime().Ticks) {throw 'Borderless helper lost its process identity or hidden startup.'}
    $vrCommand=New-DesktopBorderlessCommand $root $run $process -VrMirror
    if($vrCommand.ArgumentList[0] -cne '--borderless-vr-mirror' -or $vrCommand.UseShellExecute -or !$vrCommand.CreateNoWindow -or
        (($vrCommand.ArgumentList | Select-Object -Skip 1) -join '|') -cne (($command.ArgumentList | Select-Object -Skip 1) -join '|')) {throw 'VR mirror helper lost its separate route or process identity.'}
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/Run-Diagnostic.ps1'),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw 'Diagnostic worker has parser errors.'}
    $route=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -eq '$RuntimeSession -and $UntilExit -and $PlayerControlOnly -and $DesktopBorderless -and $process'},$true)
    if(!$route){throw 'Desktop-only user-session guard is missing.'}
    $route=[scriptblock]::Create($route.Extent.Text)
    function Start-DesktopBorderless { $script:calls++ }
    $process.Dispose()
    foreach($case in @('desktop','disabled','vr','diagnostic','bounded','missing-process')) {
        $RuntimeSession=$true;$UntilExit=$true;$PlayerControlOnly=$true;$DesktopBorderless=1;$script:calls=0
        $process=[Diagnostics.Process]::GetCurrentProcess()
        switch($case) {
            'disabled' {$DesktopBorderless=0}
            'vr' {$PlayerControlOnly=$false}
            'diagnostic' {$RuntimeSession=$false}
            'bounded' {$UntilExit=$false}
            'missing-process' {$process.Dispose();$process=$null}
        }
        & $route
        if($script:calls -ne $(if($case -eq 'desktop'){1}else{0})){throw "Borderless routed incorrectly: $case"}
        if($process){$process.Dispose()}
    }
    $vrRoute=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -eq '$RuntimeSession -and $UntilExit -and $XrProjection -and !$PlayerControlOnly -and $VrMirrorBorderless -and $process'},$true)
    if(!$vrRoute){throw 'Opt-in VR mirror routing guard is missing.'}
    $vrRoute=[scriptblock]::Create($vrRoute.Extent.Text)
    function Start-VrMirrorBorderless { $script:calls++ }
    foreach($case in @('vr','disabled','desktop','diagnostic','bounded','no-xr','missing-process')) {
        $RuntimeSession=$true;$UntilExit=$true;$PlayerControlOnly=$false;$XrProjection=$true;$VrMirrorBorderless=$true;$script:calls=0
        $process=[Diagnostics.Process]::GetCurrentProcess()
        switch($case) {
            'disabled' {$VrMirrorBorderless=$false}
            'desktop' {$PlayerControlOnly=$true}
            'diagnostic' {$RuntimeSession=$false}
            'bounded' {$UntilExit=$false}
            'no-xr' {$XrProjection=$false}
            'missing-process' {$process.Dispose();$process=$null}
        }
        & $vrRoute
        if($script:calls -ne $(if($case -eq 'vr'){1}else{0})){throw "VR mirror routed incorrectly: $case"}
        if($process){$process.Dispose()}
    }
    # Each external entrypoint rejects incompatible requests before any session work.
    foreach($script in @('Start-UserSession.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        $tree=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root ('tools/'+$script)),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw "$script has parser errors."}
        $desktopOption=$tree.ParamBlock.Parameters | Where-Object {$_.Name.VariablePath.UserPath -eq 'DesktopBorderless'}
        if(!$desktopOption -or $desktopOption.DefaultValue.SafeGetValue() -ne 1){throw "$script lost its default-on Desktop option."}
        $receipt=$tree.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text -match '^\$(receipt|preflight)\.desktopBorderlessRequested$'},$true)
        if(!$receipt){throw "$script lost its Desktop receipt."}
        $receiptExpression=[scriptblock]::Create($receipt.Right.Extent.Text)
        foreach($desktopEnabled in @(0,1)) {
            $DesktopBorderless=$desktopEnabled;$RuntimeSession=$true;$UntilExit=$true;$PlayerControlOnly=$true;$Mode='Desktop'
            if((& $receiptExpression) -ne [bool]$desktopEnabled){throw "$script ignored the Desktop toggle."}
            $PlayerControlOnly=$false;$Mode='VR'
            if((& $receiptExpression)){throw "$script marked VR as Desktop borderless."}
        }
        if(!($tree.ParamBlock.Parameters | Where-Object {$_.Name.VariablePath.UserPath -eq 'VrMirrorBorderless'})){throw "$script lost its mirror parameter."}
        $guard=$tree.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Clauses[0].Item1.Extent.Text.Contains('$VrMirrorBorderless') -and $node.Extent.Text.Contains('throw ')},$true)
        if(!$guard){throw "$script lost its mirror validation."}
        $guard=[scriptblock]::Create($guard.Extent.Text)
        $Action='Run';$Mode=if($script -eq 'Start-UserSession.ps1'){'VR'}else{'Headset'}
        $RuntimeSession=$true;$UntilExit=$true;$PlayerControlOnly=$false;$XrProjection=$true;$VrMirrorBorderless=$true;$ForceSceneResolution=$true;$SceneWidth=3840;$SceneHeight=2160
        & $guard
        if($script -eq 'Start-UserSession.ps1'){$Mode='Desktop'}else{$PlayerControlOnly=$true}
        $rejected=$false;try { & $guard } catch {$rejected=$true}
        if(!$rejected){throw "$script accepted a desktop mirror request."}
    }
    # Evaluate the actual forwarding expression on both settings. The Desktop
    # worker rebuilds its parameter table, so this must be after that branch.
    $tree=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/Run-StereoProbe.ps1'),[ref]$tokens,[ref]$errors)
    $forward=$tree.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -ceq '$parameters.DesktopBorderless'},$true)
    $playerBranch=$tree.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -ceq '$PlayerControlOnly' -and $node.Extent.Text.Contains('$parameters=@{')},$true)
    if(!$forward -or !$playerBranch -or $forward.Extent.StartOffset -le $playerBranch.Extent.EndOffset){throw 'Desktop branch can lose borderless setting.'}
    foreach($DesktopBorderless in @(0,1)) {
        $parameters=@{};& ([scriptblock]::Create($forward.Extent.Text))
        if($parameters.DesktopBorderless -ne $DesktopBorderless){throw 'Desktop borderless forwarding changed its value.'}
    }
    $tree=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/Start-CockpitTest.ps1'),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw 'Cockpit diagnostic entrypoint has parser errors.'}
    $parameterTable=$tree.Find({param($node) $node -is [Management.Automation.Language.HashtableAst] -and
        ($node.KeyValuePairs | Where-Object {$_.Item1.Extent.Text -ceq 'DesktopBorderless'})},$true)
    $value=($parameterTable.KeyValuePairs | Where-Object {$_.Item1.Extent.Text -ceq 'DesktopBorderless'}).Item2.Extent.Text
    foreach($graphics in @([pscustomobject]@{},[pscustomobject]@{DesktopBorderless=$true},[pscustomobject]@{DesktopBorderless=$false})) {
        $expected=if($null -ne $graphics.DesktopBorderless){[int]$graphics.DesktopBorderless}else{1}
        if((& ([scriptblock]::Create($value))) -ne $expected){throw 'Saved diagnostic Desktop borderless setting did not migrate or forward.'}
    }
} finally {if($process){$process.Dispose()}}
'Default-on Desktop toggle and opt-in VR mirror ownership/routing passed; no game/window was opened.'

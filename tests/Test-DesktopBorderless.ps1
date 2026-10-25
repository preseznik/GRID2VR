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
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/Run-Diagnostic.ps1'),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw 'Diagnostic worker has parser errors.'}
    $route=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Clauses[0].Item1.Extent.Text -eq '$RuntimeSession -and $UntilExit -and $PlayerControlOnly -and $process'},$true)
    if(!$route){throw 'Desktop-only user-session guard is missing.'}
    $route=[scriptblock]::Create($route.Extent.Text)
    function Start-DesktopBorderless { $script:calls++ }
    $process.Dispose()
    foreach($case in @('desktop','vr','diagnostic','bounded','missing-process')) {
        $RuntimeSession=$true;$UntilExit=$true;$PlayerControlOnly=$true;$script:calls=0
        $process=[Diagnostics.Process]::GetCurrentProcess()
        switch($case) {
            'vr' {$PlayerControlOnly=$false}
            'diagnostic' {$RuntimeSession=$false}
            'bounded' {$UntilExit=$false}
            'missing-process' {$process.Dispose();$process=$null}
        }
        & $route
        if($script:calls -ne $(if($case -eq 'desktop'){1}else{0})){throw "Borderless routed incorrectly: $case"}
        if($process){$process.Dispose()}
    }
} finally {if($process){$process.Dispose()}}
'Desktop-only owned helper routing passed; no game/window was opened.'

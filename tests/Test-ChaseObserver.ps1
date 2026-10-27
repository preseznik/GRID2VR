$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
function Ast([string]$Name) {
    $tokens=$null;$errors=$null
    $value=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$Name"),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw "$Name has parser errors: $errors"};return $value
}
$start=Ast 'Start-UserSession.ps1';$probe=Ast 'Run-StereoProbe.ps1';$run=Ast 'Run-Diagnostic.ps1'
foreach($script in @($start,$probe,$run)) {
    $binding=[scriptblock]::Create($script.ParamBlock.Extent.Text+"`n[bool]`$ChaseCameraObserver")
    $arguments=@{}
    if($script.ParamBlock.Parameters.Name.VariablePath.UserPath -contains 'HardwareSettingsFile') { $arguments.HardwareSettingsFile='fixture.xml' }
    if((& $binding @arguments) -or !(& $binding @arguments -ChaseCameraObserver)) {throw 'Observer must be opt-in.'}
}
# Execute the actual guards, not a copy of their conditions.
$ChaseFreeLook=0
foreach($script in @($start,$probe,$run)) {
    $guard=$script.FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and
        $n.Clauses[0].Item1.Extent.Text.StartsWith('$ChaseCameraObserver -and')},$false)
    if(!$guard.Count){throw 'Missing observer launch gate.'}
    $check=[scriptblock]::Create(($guard.Extent.Text -join "`n"))
    foreach($case in @('allowed','off','vr','no-proxy','no-runtime','no-human')) {
        $ChaseCameraObserver=$case -ne 'off';$Mode=if($case -eq 'vr'){'VR'}else{'Desktop'}
        $DiagnosticProxy=if($case -eq 'no-proxy'){''}else{'fixture.dll'}
        $RuntimeSession=$case -ne 'no-runtime';$UntilExit=$case -ne 'no-human';$PlayerControlOnly=$case -ne 'vr';$ProxyMode='Active'
        $rejected=$false;try{. $check}catch{$rejected=$true}
        $expected=$case -in @('vr','no-proxy') -or ($script -ne $start -and $case -in @('no-runtime','no-human'))
        if($rejected -ne $expected){throw "Incorrect launch guard for $case."}
    }
}
# Evaluate actual route dictionaries with no session or game invocation.
$ChaseCameraObserver=$true;$DiagnosticProxy='exact-candidate.dll';$Preflight=$true;$Mode='Desktop'
$runLogs=Join-Path $root 'artifacts/logs/20261027-120000-001'
$parametersStatement=$start.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and
    $n.Left.Extent.Text -ceq '$parameters' -and $n.Right.Extent.Text.Contains('ChaseCameraObserver')},$false)
if(!$parametersStatement){throw 'Missing Start-UserSession forwarding.'}
. ([scriptblock]::Create($parametersStatement.Extent.Text))
if(!$parameters.ChaseCameraObserver -or $parameters.DiagnosticProxy -cne $DiagnosticProxy -or $parameters.RunLogDirectory -cne $runLogs){throw 'Candidate selection or current run log folder was lost.'}
$forward=$probe.FindAll({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and
    $n.Left.Extent.Text -ceq '$parameters.ChaseCameraObserver'},$false)
$proxy=$probe.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and
    $n.Extent.Text.Contains('$parameters.DiagnosticProxy=$DiagnosticProxy')},$false)
if($forward.Count -ne 1 -or !$proxy){throw 'Missing worker forwarding.'}
$logForward=$probe.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and
    $n.Left.Extent.Text -ceq '$parameters.RunLogDirectory'},$false)
if(!$logForward){throw 'Missing current per-run log routing.'}
$RunLogDirectory=$runLogs
foreach($case in @('observer','textures','both','neither')) {
    $ChaseCameraObserver=$case -in @('observer','both');$CockpitTextures=if($case -in @('textures','both')){'On'}else{'Off'}
    $parameters=@{};. ([scriptblock]::Create($forward[0].Extent.Text));. ([scriptblock]::Create($proxy.Extent.Text));. ([scriptblock]::Create($logForward.Extent.Text))
    if($parameters.ChaseCameraObserver -ne $ChaseCameraObserver -or $parameters.RunLogDirectory -cne $runLogs){throw "Lost observer/log state in $case route."}
    if(($parameters.DiagnosticProxy -ceq $DiagnosticProxy) -ne ($case -ne 'neither')){throw "Current cockpit-texture and observer proxy routing conflict in $case."}
}
$fixture=Join-Path $root ('build/chase-request-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture)|Out-Null
try {
    $runPath=$fixture;$request=$run.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and
        $n.Extent.Text.Contains("'chase-camera-observer.request'")},$false)
    if(!$request){throw 'Missing bounded request writer.'}
    $run=$runPath;$ChaseCameraObserver=$true;. ([scriptblock]::Create($request.Extent.Text))
    $bytes=[IO.File]::ReadAllBytes((Join-Path $run 'chase-camera-observer.request'))
    if([Text.Encoding]::ASCII.GetString($bytes) -cne "observe-v1`n"){throw 'Native request differs.'}
} finally {
    $resolvedFixture=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($resolvedFixture) -ne (Join-Path $root 'build') -or [IO.Path]::GetFileName($resolvedFixture) -notlike 'chase-request-test-*') { throw 'Fixture cleanup path changed.' }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
Write-Output 'Observer opt-in, desktop refusal gates, current log/texture routing, exact candidate forwarding and request bytes passed; no game launched.'

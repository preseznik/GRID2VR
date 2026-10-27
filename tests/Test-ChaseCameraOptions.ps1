$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Chase-Camera.ps1')
function Read-ChaseAst([string]$Name) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$Name"),[ref]$tokens,[ref]$errors)
    if($errors.Count) { throw "$Name has parser errors: $errors" }
    return $ast
}
$names=@('ChaseFreeLook','ChaseKeepAngle','ChaseMouseMode','ChaseMaxZoomOut','ChaseMouseSensitivity','ChaseStickSensitivity','ChaseInvertVertical')
$chosen=@{ChaseFreeLook=1;ChaseKeepAngle=1;ChaseMouseMode='always';ChaseMaxZoomOut=1000;ChaseMouseSensitivity=25;ChaseStickSensitivity=300;ChaseInvertVertical=1}
foreach($script in @('Start-UserSession.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
    $ast=Read-ChaseAst $script
    # Bind the actual parameter block only. No script launch/deployment body runs.
    $bind=[scriptblock]::Create($ast.ParamBlock.Extent.Text+"`nGet-Grid2ChaseConfiguration "+(($names | ForEach-Object { '-'+$_+' $'+$_ }) -join ' '))
    $extra=if($script -eq 'Run-StereoProbe.ps1'){@{HardwareSettingsFile='unused-fixture.xml'}}else{@{}}
    if((& $bind @extra).text -cne "chase-v1 0 0 1 180 100 100 0`n") { throw "$script changed chase defaults." }
    if((& $bind @extra @chosen).text -cne "chase-v1 1 1 2 1000 25 300 1`n") { throw "$script lost chase values." }
    foreach($invalid in @(@{ChaseFreeLook=2},@{ChaseKeepAngle=-1},@{ChaseMouseMode='invalid'},@{ChaseMaxZoomOut=170},@{ChaseMaxZoomOut=181},
        @{ChaseMaxZoomOut=1010},@{ChaseMouseSensitivity=24},@{ChaseMouseSensitivity=301},@{ChaseStickSensitivity=24},@{ChaseStickSensitivity=301},@{ChaseInvertVertical=2})) {
        $rejected=$false
        try { $null=& $bind @extra @invalid } catch [Management.Automation.ParameterBindingException] { $rejected=$true }
        if(!$rejected) { throw "$script accepted invalid settings: $($invalid | ConvertTo-Json -Compress)" }
    }
    $conflict=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Clauses[0].Item1.Extent.Text -eq '$ChaseCameraObserver -and $ChaseFreeLook'},$false)
    if(!$conflict) { throw "$script lacks observer exclusion." }
    $ChaseCameraObserver=$true; $ChaseFreeLook=1; $rejected=$false
    try { & ([scriptblock]::Create($conflict.Extent.Text)) } catch { $rejected=$true }
    if(!$rejected) { throw "$script permits active controls alongside the observer." }
    $ChaseFreeLook=0; & ([scriptblock]::Create($conflict.Extent.Text))
}

# Run actual routing assignments after the desktop branch replaces its argument
# map. Check every launch mode and profile route without touching profiles.
foreach($script in @('Start-UserSession.ps1','Run-StereoProbe.ps1')) {
    $ast=Read-ChaseAst $script
    $assignments=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -match '^\$parameters\.Chase(FreeLook|KeepAngle|MouseMode|MaxZoomOut|MouseSensitivity|StickSensitivity|InvertVertical)$'},$true))
    if($assignments.Count -ne 7) { throw "$script is missing chase forwarding." }
    $forward=[scriptblock]::Create(($assignments.Extent.Text -join "`n"))
    if($script -eq 'Run-StereoProbe.ps1') {
        $desktop=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Clauses[0].Item1.Extent.Text -eq '$PlayerControlOnly' -and $node.Extent.Text.Contains('$parameters=@{')},$true)
        if(!$desktop -or ($assignments | Where-Object { $_.Extent.StartOffset -lt $desktop.Extent.EndOffset })) { throw 'Desktop map replacement would discard chase settings.' }
        $desktop=[scriptblock]::Create($desktop.Extent.Text)
    }
    foreach($Mode in @('Desktop','VR')) { foreach($LaunchMode in @('Normal','Practice','Race')) { foreach($careerId in @('current','fixture-profile')) {
        $career=@{id=$careerId}; $parameters=@{CareerProfile=$career.id;NormalLaunch=($LaunchMode -eq 'Normal')}; $PlayerControlOnly=$Mode -eq 'Desktop'
        if($script -eq 'Run-StereoProbe.ps1') { . $desktop }
        foreach($name in $names) { Set-Variable -Name $name -Value $chosen[$name] }
        . $forward
        foreach($name in $names) { if($parameters[$name] -cne $chosen[$name]) { throw "$script lost $name for $Mode/$LaunchMode/$careerId." } }
    } } }
}

$fixture=Join-Path $root ('build/chase-options-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$oldCulture=[Globalization.CultureInfo]::CurrentCulture
try {
    $ast=Read-ChaseAst 'Run-Diagnostic.ps1'
    $statement=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.Contains("'chase-camera.request'")},$false)
    if(!$statement) { throw 'Native chase request writer missing.' }
    $write=[scriptblock]::Create($statement.Extent.Text)
    [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('sl-SI')
    foreach($mode in @('left','right','always')) {
        $options=$chosen.Clone(); $options.ChaseMouseMode=$mode; $chase=Get-Grid2ChaseConfiguration @options
        $run=$fixture; $RuntimeSession=$true; $ChaseCameraObserver=$false
        . $write
        $path=Join-Path $fixture 'chase-camera.request'; $bytes=[IO.File]::ReadAllBytes($path)
        if($bytes.Length -gt 64 -or [Text.Encoding]::ASCII.GetString($bytes) -cne $chase.text -or @($bytes | Where-Object { $_ -gt 127 }).Count) { throw 'Request is not bounded invariant ASCII.' }
        [IO.File]::Delete($path)
    }
    foreach($case in @('observer','non-runtime')) {
        $RuntimeSession=$case -eq 'observer'; $ChaseCameraObserver=$case -eq 'observer'
        . $write
        if([IO.File]::Exists($path)) { throw 'Passive/non-runtime run acquired an active chase request.' }
    }
} finally {
    [Globalization.CultureInfo]::CurrentCulture=$oldCulture
    if([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($fixture)) -ne (Join-Path $root 'build')) { throw 'Fixture escaped build.' }
    foreach($file in [IO.Directory]::EnumerateFiles($fixture)) { [IO.File]::Delete($file) }
    [IO.Directory]::Delete($fixture)
}
'Chase defaults, validation, all launch routes, profile-neutral forwarding, observer exclusion and bounded request passed.'

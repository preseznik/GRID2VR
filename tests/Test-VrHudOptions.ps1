$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
# Evaluate only each real parameter block: never execute the deployment/launch body.
foreach($name in @('Start-UserSession.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$name"),[ref]$tokens,[ref]$errors)
    if($errors.Count) { throw "$name has parser errors: $errors" }
    $bind=[scriptblock]::Create($ast.ParamBlock.Extent.Text+"`n@{distance=`$HudDistance;follow=`$HudFollowView;hidden=`$HudHiddenElements;performance=[bool]`$PerformanceDisplay;performanceFollow=`$PerformanceFollowView}")
    $extra=if($name -eq 'Run-StereoProbe.ps1'){@{HardwareSettingsFile='unused-fixture.xml'}}else{@{}}
    $defaults=& $bind @extra
    if($defaults.performanceFollow -ne 1) { throw "$name changed performance placement for existing users." }
    foreach($follow in @(0,1)) {
        $bound=& $bind @extra -PerformanceDisplay -PerformanceFollowView $follow
        if($bound.performanceFollow -ne $follow) { throw "$name lost performance placement." }
    }
    if($defaults.distance -ne 1 -or $defaults.follow -ne 0 -or $defaults.hidden -ne 1 -or $defaults.performance) { throw "$name changed the HUD/performance defaults." }
    foreach($distance in @(1,1.5,7.5,20)) {
        $bound=& $bind @extra -HudDistance $distance -HudFollowView 1 -HudHiddenElements 31 -PerformanceDisplay
        if($bound.distance -ne $distance -or $bound.follow -ne 1 -or $bound.hidden -ne 31 -or !$bound.performance) { throw "$name lost a HUD/performance option." }
    }
    foreach($invalid in @(@{PerformanceFollowView=2},@{PerformanceFollowView=-1},@{HudDistance=0.5},@{HudDistance=20.5},@{HudDistance=1.25},@{HudFollowView=2},@{HudHiddenElements=-1},@{HudHiddenElements=32})) {
        $rejected=$false
        try { $null=& $bind @extra @invalid } catch [Management.Automation.ParameterBindingException] { $rejected=$true }
        if(!$rejected) { throw "$name accepted invalid HUD parameters: $($invalid | ConvertTo-Json -Compress)" }
    }
}
# Exercise the actual request-writing statement, with no game/install access.
$fixture=Join-Path $root ('build/vr-hud-options-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$oldCulture=[Globalization.CultureInfo]::CurrentCulture
try {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/Run-Diagnostic.ps1'),[ref]$tokens,[ref]$errors)
    $statement=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.Contains("'vr-hud.request'")},$false)
    if(!$statement) { throw 'Missing native HUD request writer.' }
    $write=[scriptblock]::Create($statement.Extent.Text)
    $run=$fixture; $RuntimeSession=$true; $ReplayStereo=$true; $PlayerControlOnly=$false
    $HudDistance=[decimal]7.5; $HudFollowView=1; $HudHiddenElements=31
    [Globalization.CultureInfo]::CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('sl-SI')
    & $write
    $request=Join-Path $fixture 'vr-hud.request'
    if([IO.File]::ReadAllText($request) -cne '7.5 1 31') { throw 'HUD request is not invariant ASCII or lost its settings.' }
    [IO.File]::Delete($request)
    $PlayerControlOnly=$true
    & $write
    if([IO.File]::Exists($request)) { throw 'Desktop-only session acquired a VR HUD request.' }
    $statement=$ast.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.StartsWith('if ($PerformanceDisplay)')},$false)
    if(!$statement) { throw 'Missing performance request writer.' }
    $write=[scriptblock]::Create($statement.Extent.Text)
    foreach($enabled in @($false,$true)) {
        foreach($follow in @(0,1)) {
            $PerformanceDisplay=$enabled; $PerformanceFollowView=$follow
            & $write
            $request=Join-Path $fixture 'performance-display.request'
            $fixed=Join-Path $fixture 'performance-display-fixed.request'
            if([IO.File]::Exists($request) -ne $enabled -or [IO.File]::Exists($fixed) -ne ($enabled -and !$follow)) { throw 'Incorrect performance placement request.' }
            [IO.File]::Delete($request); [IO.File]::Delete($fixed)
        }
    }
} finally {
    [Globalization.CultureInfo]::CurrentCulture=$oldCulture
    if([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($fixture)) -ne (Join-Path $root 'build')) { throw 'Fixture escaped build.' }
    foreach($file in [IO.Directory]::EnumerateFiles($fixture)) { [IO.File]::Delete($file) }
    [IO.Directory]::Delete($fixture)
}
'VR HUD parameter validation, preserved defaults, invariant request and desktop isolation passed.'

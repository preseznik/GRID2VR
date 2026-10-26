$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
foreach($name in @('Start-UserSession','Run-StereoProbe','Run-Diagnostic')) {
    $errors=$null;$tokens=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$name.ps1"),[ref]$tokens,[ref]$errors)
    if($errors.Count) { throw "$name parse failure: $errors" }
    $parameter=$ast.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -ceq 'CockpitTextures' }
    if(!$parameter -or $parameter.DefaultValue.Value -cne 'Off' -or !$parameter.Extent.Text.Contains("ValidateSet('Off','Observe','Replace')")) { throw 'Missing opt-in validation.' }
    if($name -ne 'Run-Diagnostic') {
        $assign=$ast.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -ceq '$parameters.CockpitTextures'},$true)
        if(!$assign) { throw 'Missing worker forwarding.' }
        foreach($mode in @('Off','Observe','Replace')) {
            $parameters=@{};$CockpitTextures=$mode;. ([scriptblock]::Create($assign.Extent.Text))
            if($parameters.CockpitTextures -cne $mode) { throw 'Lost texture mode.' }
        }
        if($name -eq 'Start-UserSession') {
            $choice=$ast.Find({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and
                $n.Left.Extent.Text -ceq '$DiagnosticProxy' -and $n.Right.Extent.Text.Contains('Get-LauncherVrPreviewProxy')},$true)
            if(!$choice) { throw 'Missing preview renderer selection.' }
            function Get-Grid2Proxy($Root) { throw 'Unverified renderer route used.' }
            function Get-LauncherVrPreviewProxy($Root) { 'verified-preview-bundle' }
            foreach($mode in @('Off','Observe','Replace')) {
                $CockpitTextures=$mode;. ([scriptblock]::Create($choice.Extent.Text))
                if($DiagnosticProxy -cne 'verified-preview-bundle') { throw 'Texture choice changed the VR renderer.' }
            }
            function Get-LauncherVrPreviewProxy($Root) { throw 'fixture integrity failure' }
            foreach($mode in @('Off','Observe','Replace')) {
                $CockpitTextures=$mode;$rejected=$false
                try { . ([scriptblock]::Create($choice.Extent.Text)) }
                catch { if($_.Exception.Message -ne 'fixture integrity failure') { throw };$rejected=$true }
                if(!$rejected) { throw 'Texture mode bypassed renderer integrity checks.' }
            }
        }
    } else { $diagnostic=$ast }
}
$fixture=Join-Path $root ('build/focus-request-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture)|Out-Null
try {
    $request=$diagnostic.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.Contains("'cockpit-textures.request'")},$false)
    if(!$request) { throw 'Missing native request writer.' }
    $run=$fixture;$CockpitTextures='Off';. ([scriptblock]::Create($request.Extent.Text))
    if(Test-Path -LiteralPath (Join-Path $run 'cockpit-textures.request')) { throw 'Off wrote a request.' }
    foreach($mode in @('Observe','Replace')) {
        $CockpitTextures=$mode;. ([scriptblock]::Create($request.Extent.Text))
        $bytes=[IO.File]::ReadAllBytes((Join-Path $run 'cockpit-textures.request'))
        if([Text.Encoding]::ASCII.GetString($bytes) -cne $mode) { throw 'Incorrect request bytes.' }
    }
} finally {
    [IO.File]::Delete((Join-Path $fixture 'cockpit-textures.request'));[IO.Directory]::Delete($fixture,$false)
}
Write-Output 'Cockpit texture defaults, desktop/VR forwarding and exact native request checks passed; no game launched.'

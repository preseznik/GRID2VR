$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Fake-Vr.ps1')
function Check([bool]$Value,[string]$Message) { if(!$Value) { throw $Message } }
$fixture=Join-Path $root ('build/fake-vr-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    # SteamVR's own file: unrelated sections survive, the null driver is forced and standby is off.
    $path=Join-Path $fixture 'steamvr.vrsettings'
    Set-Content -LiteralPath $path -Value '{"DesktopUI":{"pairing":"1,2,3,4,0"},"steamvr":{"forcedDriver":"","mirrorViewGeometry":"0 0 1 1"}}' -NoNewline
    Set-FakeVrNullDriver $path 1600 1800
    $settings=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
    Check ($settings['steamvr']['forcedDriver'] -ceq 'null' -and $settings['steamvr']['activateMultipleDrivers'] -eq $true) 'Null driver not forced.'
    Check ($settings['steamvr']['mirrorViewGeometry'] -ceq '0 0 1 1' -and $settings['DesktopUI']['pairing'] -ceq '1,2,3,4,0') 'Unrelated SteamVR settings changed.'
    Check ($settings['power']['turnOffScreensTimeout'] -eq 86400 -and $settings['power']['pauseCompositorOnStandby'] -eq $false) 'Standby still pauses the compositor.'
    Check ($settings['steamvr']['enableHomeApp'] -eq $false -and $settings['dashboard']['enableDashboard'] -eq $false -and
        $settings['dashboard']['autoShowGameTheater'] -eq $false) 'SteamVR Home or the dashboard could keep the session unfocused.'
    Check ($settings['driver_null']['renderWidth'] -eq 1600 -and $settings['driver_null']['renderHeight'] -eq 1800 -and $settings['driver_null']['enable'] -eq $true) 'Virtual headset size not applied.'
    # A minimal file without steamvr or power sections still gets both.
    Set-Content -LiteralPath $path -Value '{}' -NoNewline
    Set-FakeVrNullDriver $path
    $settings=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
    Check ($settings['steamvr']['forcedDriver'] -ceq 'null' -and $settings['driver_null']['renderWidth'] -eq 2016) 'Minimal settings not completed.'
    # Only a renderer with the zero-adapter fix is accepted for a virtual-headset run.
    $renderer=Join-Path $fixture 'd3d11.dll'
    [IO.File]::WriteAllBytes($renderer,[Text.Encoding]::ASCII.GetBytes("MZ..runtime names no graphics adapter; using the game's.."))
    Check (Test-FakeVrRenderer $renderer) 'Renderer with the adapter fix refused.'
    [IO.File]::WriteAllBytes($renderer,[Text.Encoding]::ASCII.GetBytes('MZ..device game=%p compatible=%u..'))
    Check (!(Test-FakeVrRenderer $renderer) -and !(Test-FakeVrRenderer (Join-Path $fixture 'missing.dll'))) 'Renderer without the adapter fix accepted.'
    # -HeadMotion also needs the renderer's diagnostic sweep.
    [IO.File]::WriteAllBytes($renderer,[Text.Encoding]::ASCII.GetBytes("MZ..runtime names no graphics adapter; using the game's..head_motion_requires_virtual_headset.."))
    Check ((Test-FakeVrRenderer $renderer -HeadMotion) -and (Test-FakeVrRenderer $renderer)) 'Renderer with head motion refused.'
    [IO.File]::WriteAllBytes($renderer,[Text.Encoding]::ASCII.GetBytes("MZ..runtime names no graphics adapter; using the game's.."))
    Check (!(Test-FakeVrRenderer $renderer -HeadMotion)) 'Renderer without head motion accepted for -HeadMotion.'
    # The script's headset-name check matches the renderer's (head_motion.h VirtualHeadset).
    Check ((Test-FakeVrHeadsetName 'SteamVR/OpenXR : null') -and (Test-FakeVrHeadsetName 'SteamVR/OpenXR : NULL ')) 'Virtual headset name refused.'
    foreach($name in @('SteamVR/OpenXR : lighthouse','SteamVR/OpenXR : oculus','SteamVR/OpenXR : nullx','null','VirtualDesktopXR : null','',$null)) {
        Check (!(Test-FakeVrHeadsetName $name)) "Real headset name accepted: $name"
    }
    # Run-FakeVr -> Run-AutomatedFlicker -> Run-StereoProbe -> Run-Diagnostic are plain scripts:
    # a splatted name the next one does not declare is silently dropped (as Online was in 0.2.16).
    function Read-ScriptAst([string]$Name) {
        $tokens=$null;$errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$Name"),[ref]$tokens,[ref]$errors)
        if($errors.Count) { throw "$Name has parser errors: $errors" }
        return $ast
    }
    function Get-ScriptParameters($Ast) { @($Ast.ParamBlock.Parameters | ForEach-Object { $_.Name.VariablePath.UserPath }) }
    function Get-SplatNames($Ast,[string]$Variable) {
        $names=@($Ast.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq $Variable},$true) |
            ForEach-Object { $_.Right.Find({param($node) $node -is [Management.Automation.Language.HashtableAst]},$true).KeyValuePairs } | ForEach-Object { $_.Item1.Extent.Text })
        $names+=@($Ast.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left -is [Management.Automation.Language.MemberExpressionAst] -and
            $node.Left.Expression.Extent.Text -eq $Variable},$true) | ForEach-Object { $_.Left.Member.Extent.Text })
        return $names
    }
    $flickerAst=Read-ScriptAst 'Run-AutomatedFlicker.ps1';$probeAst=Read-ScriptAst 'Run-StereoProbe.ps1';$diagnosticAst=Read-ScriptAst 'Run-Diagnostic.ps1'
    $fakeAst=Read-ScriptAst 'Run-FakeVr.ps1'
    $fakeBind=[scriptblock]::Create($fakeAst.ParamBlock.Extent.Text+"`n`$XrRuntime")
    $flickerBind=[scriptblock]::Create($flickerAst.ParamBlock.Extent.Text+"`n`$XrRuntime")
    foreach($runtime in 'SteamVR','Default') {
        Check ((& $fakeBind -Evidence $fixture -DiagnosticProxy $renderer -XrRuntime $runtime) -eq $runtime) 'Fake-VR runtime binding lost.'
        Check ((& $flickerBind -DiagnosticProxy $renderer -XrRuntime $runtime) -eq $runtime) 'Automated VR runtime binding lost.'
    }
    foreach($hop in @(@{from=(Read-ScriptAst 'Run-FakeVr.ps1');variable='$run';to=$flickerAst;name='Run-AutomatedFlicker.ps1'},
            @{from=$flickerAst;variable='$parameters';to=$probeAst;name='Run-StereoProbe.ps1'})) {
        $names=Get-SplatNames $hop.from $hop.variable;$declared=Get-ScriptParameters $hop.to
        $missing=@($names | Where-Object { $_ -notin $declared } | Sort-Object -Unique)
        Check ($names.Count -ge 10 -and $names -contains 'HeadMotion') "Fake-VR forwarding scan to $($hop.name) found too few options."
        Check (!$missing.Count) "$($hop.name) silently drops fake-VR options: $($missing -join ', ')"
    }
    $headsetCall=$probeAst.Find({param($node) $node -is [Management.Automation.Language.HashtableAst] -and @($node.KeyValuePairs | ForEach-Object { $_.Item1.Extent.Text }) -contains 'XrProjection'},$true)
    Check ($headsetCall -and @($headsetCall.KeyValuePairs | ForEach-Object { $_.Item1.Extent.Text }) -contains 'HeadMotion' -and (Get-ScriptParameters $diagnosticAst) -contains 'HeadMotion') 'Run-StereoProbe.ps1 drops HeadMotion before Run-Diagnostic.ps1.'
    Write-Output 'Fake VR: null-driver settings, unrelated settings kept, standby off, renderer gates and option hand-offs passed.'
} finally { Remove-Item -LiteralPath $fixture -Recurse -Force }

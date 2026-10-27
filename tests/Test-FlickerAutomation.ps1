$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Flicker-Automation.ps1')
$fixture=Join-Path $root ('build/flicker-automation-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
function Check([string]$Expected,[bool]$Requested=$false) {
    $actual=Get-FlickerAutomationDecision $fixture 120 $Requested
    if($actual -cne $Expected) { throw "Expected $Expected, got $actual" }
}
function Status($State,$Accepted,$Attempted,$Failed=$false) {
    @{state=$State;acceptedPairs=$Accepted;attemptedPairs=$Attempted;failed=$Failed} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'runtime-status.json')
}
function Reject-SignInvocation([string]$Script,[hashtable]$Parameters,[string]$Expected) {
    # Only the launch-independent options helper is supplied. Missing session
    # dependencies still prevent a guard regression from reaching the game.
    $message=''
    try { & (Join-Path $fixture $Script) @Parameters | Out-Null } catch { $message=$_.Exception.Message }
    if($message -notlike $Expected) { throw "Unexpected sign-target validation for ${Script}: $message" }
}
try {
    Copy-Item -LiteralPath (Join-Path $root 'tools/Chase-Camera.ps1') -Destination (Join-Path $fixture 'Chase-Camera.ps1')
    foreach($name in @('Run-AutomatedFlicker.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1','Start-UserSession.ps1')) {
        $script=Join-Path $root "tools/$name"
        $tokens=$null;$parseErrors=$null
        [Management.Automation.Language.Parser]::ParseFile($script,[ref]$tokens,[ref]$parseErrors) | Out-Null
        if($parseErrors.Count) { throw "Script parse failed: $name" }
        Copy-Item -LiteralPath $script -Destination (Join-Path $fixture $name)
    }
    Reject-SignInvocation 'Start-UserSession.ps1' @{HeadsetVisibilityCandidate=$true} '*requires VR and an explicit isolated proxy*'
    Reject-SignInvocation 'Start-UserSession.ps1' @{HeadsetVisibilityCandidate=$true;Mode='Desktop';DiagnosticProxy='fixture.dll'} '*requires VR and an explicit isolated proxy*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{ObservationSeconds=60;Mode='Headset';TrackedVisibility=$true;VisibilityCapture=$true;TimeoutSeconds=180} '*Extended observation requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{ObservationSeconds=60;TrackedVisibility=$true;VisibilityCapture=$true;TimeoutSeconds=100} '*Extended observation requires*'
    foreach($script in @('Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        Reject-SignInvocation $script @{FlickerObservationSeconds=60} '*Extended observation requires*'
        Reject-SignInvocation $script @{FlickerObservationSeconds=60;TrackedVisibility=$true;VisibilityCapture=$true;AutoCaptureFlicker=$true;RuntimeSession=$true;HeadsetVisibilityCandidate=$true;TimeoutSeconds=180} '*Extended observation requires*'
    }
    foreach($script in @('Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        $candidate=@{HeadsetVisibilityCandidate=$true;RuntimeSession=$true;UntilExit=$true;
            FlickerDiagnostic=$true;TrackedVisibility=$true;MirrorPreview=$true;VisibilityCapture=$true;
            InstanceCapacity='Expand';DiagnosticProxy='fixture.dll'}
        if($script -eq 'Run-StereoProbe.ps1') { $candidate.Mode='Headset' }
        else {
            $candidate.XrSessionProbe=$true; $candidate.XrProjection=$true; $candidate.ReplayStereo=$true
            $candidate.ReplayFullPose=$true; $candidate.ReplayAsymmetricFov=$true; $candidate.ExpandVisibility=$true
        }
        foreach($required in @($candidate.Keys | Where-Object { $_ -ne 'HeadsetVisibilityCandidate' })) {
            $invalid=$candidate.Clone(); $invalid.Remove($required)
            # Preflight is also safe and intentionally accepted by the top-level gate.
            if($required -eq 'Mode') { $invalid.Mode='Desktop' }
            Reject-SignInvocation $script $invalid '*requires isolated live-XR human practice*'
        }
        foreach($forbidden in @('AutoCaptureFlicker','RecordedViewsFile','NativeVisibilityControl','FlickerSignTarget','PlayerControlOnly','PlayerControlProbe','WaitForStereoStart')) {
            $invalid=$candidate.Clone()
            $invalid[$forbidden]=switch($forbidden) { RecordedViewsFile {'fixture-views'} PlayerControlProbe {'Human'} default {$true} }
            Reject-SignInvocation $script $invalid '*requires isolated live-XR human practice*'
        }
    }
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{CaptureSignTransition=$true;Mode='Headset'} '*unattended simulated AI run*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{VisibilityCapture=$true} '*Visibility-only capture requires*'
    foreach($name in @('Run-AutomatedFlicker.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        Reject-SignInvocation $name @{InstanceCapacity='Observe'} '*Instance-capacity diagnostics require*'
        Reject-SignInvocation $name @{InstanceCapacity='Observe';TrackedVisibility=$true} '*Instance-capacity diagnostics require*'
        Reject-SignInvocation $name @{InstanceCapacity='Expand'} '*Instance-capacity diagnostics require*'
        Reject-SignInvocation $name @{InstanceCapacity='Expand';TrackedVisibility=$true} '*Instance-capacity diagnostics require*'
    }
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{VisibilityCapture=$true;TrackedVisibility=$true;CaptureSignTransition=$true} '*Visibility-only capture requires*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{VisibilityCapture=$true} '*Visibility-only capture requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{VisibilityCapture=$true} '*Visibility-only capture requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{VisibilityCapture=$true;TrackedVisibility=$true;Mode='Headset'} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{VisibilityCapture=$true;NativeVisibilityControl=$true;Mode='Headset'} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{CaptureSignTransition=$true;Preflight=$true} '*unattended simulated AI run*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{CaptureSignTransition=$true;AfterPairs=480} '*after the first pair*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{FlickerSignTarget=$true;Mode='Headset';RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true} '*unattended simulated AI flicker*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{FlickerSignTarget=$true;Mode='Desktop';RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$false} '*unattended simulated AI flicker*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{FlickerSignTarget=$true;Mode='Desktop';RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;FlickerCaptureAfterPairs=2} '*after the first stereo pair*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';XrProjection=$true} '*unattended simulated AI flicker*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';PlayerControlProbe='Human'} '*unattended simulated AI flicker*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';FlickerCaptureAfterPairs=2} '*after the first stereo pair*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{NativeVisibilityControl=$true;Preflight=$true} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{NativeVisibilityControl=$true;CaptureSignTransition=$true;Mode='Headset'} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{NativeVisibilityControl=$true;Mode='Desktop';RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$false} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{NativeVisibilityControl=$true;FlickerSignTarget=$true;Mode='Desktop';RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;UntilExit=$true} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{NativeVisibilityControl=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$false;ReplayStereo=$true;RecordedViewsFile='fixture-views'} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{NativeVisibilityControl=$true;FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';XrSessionProbe=$true} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{NativeVisibilityControl=$true;FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';PlayerControlProbe='Human'} '*Native-visibility control requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{TrackedVisibility=$true;Preflight=$true} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{TrackedVisibility=$true;NativeVisibilityControl=$true;CaptureSignTransition=$true} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{TrackedVisibility=$true;CaptureSignTransition=$true;Mode='Headset'} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{TrackedVisibility=$true;FlickerSignTarget=$true;Mode='Headset';RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{TrackedVisibility=$true;NativeVisibilityControl=$true;FlickerSignTarget=$true;Mode='Desktop';RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{TrackedVisibility=$true;FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';ReplayFullPose=$true;ReplayAsymmetricFov=$true;ExpandVisibility=$true;XrProjection=$true} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{TrackedVisibility=$true;FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';ReplayFullPose=$true;ReplayAsymmetricFov=$true} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{TrackedVisibility=$true;FlickerSignTarget=$true;RuntimeSession=$true;FlickerDiagnostic=$true;AutoCaptureFlicker=$true;ReplayStereo=$true;RecordedViewsFile='fixture-views';ReplayFullPose=$true;ReplayAsymmetricFov=$true;ExpandVisibility=$true;PlayerControlProbe='Human'} '*Tracked visibility requires*'
    foreach($entry in @('Run-AutomatedFlicker.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        Reject-SignInvocation $entry @{MirrorPreview=$true} '*Mirror preview requires*'
    }
    Reject-SignInvocation 'Run-AutomatedFlicker.ps1' @{MirrorPreview=$true;TrackedVisibility=$true;Mode='Headset'} '*Tracked visibility requires*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{RemoveCinemaMatte=$true;Mode='Desktop';RuntimeSession=$true} '*Cinema matte removal requires*'
    Reject-SignInvocation 'Run-StereoProbe.ps1' @{RemoveCinemaMatte=$true;Mode='Preflight'} '*Cinema matte removal requires*'
    Reject-SignInvocation 'Run-Diagnostic.ps1' @{RemoveCinemaMatte=$true;RuntimeSession=$true;ReplayStereo=$true} '*Cinema matte removal requires*'
    Check 'wait'
    Status 0 119 119; Check 'wait'
    Status 1 120 120; Check 'wait'
    Status 0 120 121; Check 'wait'
    Status 0 120 120; Check 'capture'; Check 'wait' $true
    $log=Join-Path $fixture 'flicker-draws.jsonl'
    [IO.File]::WriteAllText($log,'{"type":"complete","accepted":true}')
    Check 'wait' $true
    [IO.File]::WriteAllText($log,"{`"type`":`"end`",`"scenes`":8}`n")
    Check 'complete' $true
    [IO.File]::WriteAllText((Join-Path $fixture 'flicker-watch.request'),'watch')
    Check 'wait' $true
    $watch=Join-Path $fixture 'flicker-watch.jsonl'
    [IO.File]::WriteAllText($watch,'{"type":"scene","scene":10}')
    Check 'wait' $true
    [IO.File]::WriteAllText($watch,'{"type":"end","scenes":180,"accepted":180,"reason":"complete","truncated":false}')
    Check 'complete' $true
    [IO.File]::WriteAllText($watch,'{"type":"end","scenes":240,"accepted":179,"reason":"scene_budget","truncated":false}')
    Check 'capture_truncated' $true
    [IO.File]::WriteAllText($watch,'{"type":"end","scenes":180,"accepted":180,"reason":"complete","truncated":true}')
    Check 'capture_truncated' $true
    [IO.File]::WriteAllText($watch,'{"type":"end",')
    Check 'wait' $true
    # Failed renderer wins over a completed burst; never report healthy capture.
    Status 3 120 121 $true; Check 'runtime_failed' $true
    Status 0 120 120
    [IO.File]::WriteAllText($log,'{"type":"end","scenes":7}')
    Check 'wait' $true
    [IO.File]::WriteAllText($log,'{"type":"truncated","reason":"log_budget"}')
    Check 'capture_truncated' $true
    [IO.File]::WriteAllText($log,'{"type":"end",')
    Check 'wait' $true
    [IO.File]::WriteAllText((Join-Path $fixture 'runtime-status.json'),'{"frame":')
    Check 'wait'
    [IO.File]::WriteAllText((Join-Path $fixture 'runtime-status.json'),'x'*4097)
    Check 'invalid_status'
    $runtime=@{storageBounded=$true;status=@{failed=$false;state=2;acceptedPairs=128;attemptedPairs=128};stopped=@{failed=$false};unexpectedFaults=@()}
    $capture=@{complete=$true;logTruncated=$false;malformedLines=$false;scenes=@(1..8 | ForEach-Object { @{completion=@{accepted=1;truncated=@(0,0,0)};drawCounts=@(100,100,100)} })}
    Assert-AutomatedFlickerResult $runtime $capture
    foreach($fault in @('partial','truncated','unacknowledged','failed','missing_eye')) {
        $r=$runtime | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        $c=$capture | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        switch($fault) {
            partial { $c.scenes=$c.scenes[0..6] }
            truncated { $c.scenes[0].completion.truncated=@(0,1,0) }
            unacknowledged { $r.stopped=$null }
            failed { $r.status.failed=$true }
            missing_eye { $c.scenes[0].drawCounts=@(100,100,0) }
        }
        $rejected=$false
        try { Assert-AutomatedFlickerResult $r $c } catch { $rejected=$true }
        if(!$rejected) { throw "Unsafe automatic capture accepted: $fault" }
    }
    Write-Output 'Automatic capture: sign-target/native-visibility safety gates, warmup, no repeated trigger, partial writes, completion and failure gates passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($resolved) -ne (Join-Path $root 'build') -or [IO.Path]::GetFileName($resolved) -notmatch '^flicker-automation-[0-9a-f]{32}$') { throw 'Fixture escaped workspace.' }
    if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
    Remove-Item -LiteralPath $resolved -Recurse
}

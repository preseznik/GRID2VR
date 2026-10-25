# Read only this owned run's bounded telemetry. Decisions never synthesize input.
function Get-FlickerWatchDecision([string]$Run) {
    if(!(Test-Path -LiteralPath (Join-Path $Run 'flicker-watch.request'))) { return 'complete' }
    $path=Join-Path $Run 'flicker-watch.jsonl'
    if(!(Test-Path -LiteralPath $path)) { return 'wait' }
    $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        if($stream.Length -gt 16MB) { return 'capture_truncated' }
        $count=[int][Math]::Min(4096,$stream.Length)
        $null=$stream.Seek(-$count,[IO.SeekOrigin]::End)
        $bytes=New-Object byte[] $count
        $read=$stream.Read($bytes,0,$count)
        $lines=[Text.Encoding]::UTF8.GetString($bytes,0,$read).Trim().Split("`n")
        try { $last=$lines[-1] | ConvertFrom-Json } catch { return 'wait' }
        if($last.type -eq 'truncated') { return 'capture_truncated' }
        if($last.type -eq 'end') {
            if($last.reason -eq 'complete' -and $last.accepted -eq 180 -and $last.scenes -eq 180 -and $last.truncated -eq $false) { return 'complete' }
            return 'capture_truncated'
        }
    } finally { $stream.Dispose() }
    return 'wait'
}
function Get-FlickerAutomationDecision([string]$Run, [int]$AfterPairs, [bool]$Requested) {
    $statusPath=Join-Path $Run 'runtime-status.json'
    if(Test-Path -LiteralPath $statusPath) {
        if((Get-Item -LiteralPath $statusPath).Length -gt 4096) { return 'invalid_status' }
        # The render thread replaces this small file; a partial write is not a failure.
        try { $status=Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json } catch { return 'wait' }
        if($status.failed -eq $true) { return 'runtime_failed' }
        if(!$Requested -and $status.failed -eq $false -and $status.state -eq 0 -and
            $status.acceptedPairs -ge $AfterPairs -and $status.attemptedPairs -eq $status.acceptedPairs) { return 'capture' }
    }
    if(!$Requested) { return 'wait' }
    $logPath=Join-Path $Run 'flicker-draws.jsonl'
    if(!(Test-Path -LiteralPath $logPath)) { return 'wait' }
    # Read at most 4 KiB, not the entire draw log at each half-second poll.
    $stream=[IO.File]::Open($logPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        if($stream.Length -gt 32MB) { return 'capture_truncated' }
        $count=[int][Math]::Min(4096,$stream.Length)
        $null=$stream.Seek(-$count,[IO.SeekOrigin]::End)
        $bytes=New-Object byte[] $count
        $read=$stream.Read($bytes,0,$count)
        $lines=[Text.Encoding]::UTF8.GetString($bytes,0,$read).Trim().Split("`n")
        try { $last=$lines[-1] | ConvertFrom-Json } catch { return 'wait' }
        if($last.type -eq 'truncated') { return 'capture_truncated' }
        if($last.type -eq 'end' -and $last.scenes -eq 8) { return Get-FlickerWatchDecision $Run }
    } finally { $stream.Dispose() }
    return 'wait'
}

function Assert-AutomatedFlickerResult($Runtime, $Capture) {
    # This job validates captured render data, not focus-loss gameplay. Attract
    # mode can capture before the separate background-play hook becomes ready.
    if(!$Runtime -or !$Capture -or $Runtime.storageBounded -ne $true -or
        $Runtime.status.failed -ne $false -or $Runtime.status.state -ne 2 -or
        $Runtime.status.acceptedPairs -le 0 -or $Runtime.status.attemptedPairs -ne $Runtime.status.acceptedPairs -or
        !$Runtime.stopped -or $Runtime.stopped.failed -ne $false -or @($Runtime.unexpectedFaults).Count -gt 0 -or
        $Capture.complete -ne $true -or $Capture.logTruncated -ne $false -or $Capture.malformedLines -ne $false -or
        @($Capture.scenes).Count -ne 8) { throw 'Incomplete or unhealthy automatic stereo capture.' }
    foreach($scene in $Capture.scenes) {
        if(!$scene.completion -or $scene.completion.accepted -ne 1 -or @($scene.completion.truncated).Count -ne 3 -or
            @($scene.completion.truncated | Where-Object { $_ -ne 0 }).Count -gt 0 -or
            @($scene.drawCounts).Count -ne 3 -or @($scene.drawCounts | Where-Object { $_ -le 0 }).Count -gt 0) {
            throw 'Automatic capture contains an incomplete or truncated eye scene.'
        }
    }
}

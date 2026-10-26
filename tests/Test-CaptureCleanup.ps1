param([string]$PythonPath='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$artifacts=[IO.Path]::GetFullPath((Join-Path $root 'artifacts'))
$finish=Join-Path $root 'tools\Finish-Diagnostic.ps1'
$fixtures=[Collections.Generic.List[string]]::new()
function New-Fixture {
    param([switch]$Grouped)
    $run=Join-Path $artifacts ($(if($Grouped){'logs/20990101-'}else{'diagnostic-20990101-'})+(Get-Date -Format 'HHmmss-fff'))
    New-Item -ItemType Directory -Path $run | Out-Null
    $fixtures.Add($run)
    $game=Join-Path $run 'game'
    New-Item -ItemType Directory -Path $game | Out-Null
    @{result='process_exited';replayOnce=$true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'session.json')
    @{run=$run;gameRoot=$game} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'deployment-restored.json')
    @{accepted=$false;failedGates=@('identicalSourceReadback')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'replay-summary.json')
    [IO.File]::WriteAllText((Join-Path $run 'frame-1.raw'),'disposable captured bytes')
    [IO.File]::WriteAllText((Join-Path $run 'protected-0.bin'),'recovery backup')
    [IO.File]::WriteAllText((Join-Path $run 'user-file.raw'),'unrelated data')
    return $run
}
function Assert-Rejected([scriptblock]$body,[string]$label) {
    $rejected=$false
    try { & $body | Out-Null } catch { $rejected=$true }
    if (!$rejected) { throw "$label was not rejected" }
}
try {
    $grouped=New-Fixture -Grouped
    New-Item -ItemType Directory -Path (Join-Path $grouped 'backup'),(Join-Path $grouped 'selection') | Out-Null
    [IO.File]::WriteAllText((Join-Path $grouped 'backup/protected-0.bin'),'grouped recovery backup')
    [IO.File]::WriteAllText((Join-Path $grouped 'user-session.log'),'worker log')
    & $finish -RunPath $grouped -PythonPath $PythonPath
    if(Test-Path -LiteralPath (Join-Path $grouped 'frame-1.raw')) { throw 'Grouped capture cleanup failed.' }
    foreach($name in @('backup/protected-0.bin','user-session.log','selection')) {
        if(!(Test-Path -LiteralPath (Join-Path $grouped $name))) { throw 'Grouped diagnostics or recovery data was removed.' }
    }
    $run=New-Fixture
    Push-Location $root
    try { & $finish -RunPath (Join-Path 'artifacts' (Split-Path $run -Leaf)) -Action Plan -PythonPath $PythonPath }
    finally { Pop-Location }
    if (!(Test-Path -LiteralPath (Join-Path $run 'frame-1.raw'))) { throw 'Plan deleted a capture.' }
    & $finish -RunPath $run -Action Apply
    if (Test-Path -LiteralPath (Join-Path $run 'frame-1.raw')) { throw 'Planned capture was retained.' }
    foreach ($name in @('protected-0.bin','user-file.raw','replay-summary.json','session.json','deployment-restored.json')) {
        if (!(Test-Path -LiteralPath (Join-Path $run $name))) { throw "Protected file removed: $name" }
    }
    $retained=Get-Content -LiteralPath (Join-Path $run 'retained-evidence.json') -Raw | ConvertFrom-Json
    if ($retained.reports.'replay-summary.json'.result.accepted -ne $false) { throw 'Failed replay evidence changed.' }
    $receipt=Get-Content -LiteralPath (Join-Path $run 'capture-cleanup.json') -Raw | ConvertFrom-Json
    if ($receipt.status -ne 'complete' -or $receipt.removed.Count -ne 1 -or $receipt.removedBytes -le 0) { throw 'Cleanup receipt incomplete.' }
    & $finish -RunPath $run # Idempotent; must not overwrite the original analysis.

    $empty=New-Fixture
    Remove-Item -LiteralPath (Join-Path $empty 'frame-1.raw')
    & $finish -RunPath $empty -PythonPath $PythonPath
    $emptyReceipt=Get-Content -LiteralPath (Join-Path $empty 'capture-cleanup.json') -Raw | ConvertFrom-Json
    if ($emptyReceipt.removedBytes -ne 0 -or $emptyReceipt.status -ne 'complete') { throw 'Empty capture cleanup failed.' }

    $notStarted=New-Fixture
    Remove-Item -LiteralPath (Join-Path $notStarted 'frame-1.raw')
    Remove-Item -LiteralPath (Join-Path $notStarted 'replay-summary.json')
    @{result='no_game_process';replayOnce=$true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $notStarted 'session.json')
    foreach ($name in @('repeat-shadow.request','normalize-shadow-bindings.request')) {
        [IO.File]::WriteAllText((Join-Path $notStarted $name),'requested, but the game never started')
    }
    & $finish -RunPath $notStarted -PythonPath $PythonPath
    $missing=Get-Content -LiteralPath (Join-Path $notStarted 'shadow-repeat-summary.json') -Raw | ConvertFrom-Json
    if ($missing.complete -or $missing.restored) { throw 'Missing game trace became accepted control evidence.' }
    $notStartedReceipt=Get-Content -LiteralPath (Join-Path $notStarted 'capture-cleanup.json') -Raw | ConvertFrom-Json
    if ($notStartedReceipt.status -ne 'complete' -or $notStartedReceipt.removedBytes -ne 0) { throw 'No-game cleanup failed.' }

    $runtime=New-Fixture
    @{result='process_exited';runtimeSession=$true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runtime 'session.json')
    [IO.File]::WriteAllBytes((Join-Path $runtime 'stage-5-flicker-eye1.ppm'),([Text.Encoding]::ASCII.GetBytes("P6`n1 1`n255`n")+[byte[]](255,0,0)))
    '{"type":"image","scene":5,"eye":1,"file":"stage-5-flicker-eye1.ppm","status":"saved"}' | Set-Content -LiteralPath (Join-Path $runtime 'flicker-draws.jsonl')
    & $finish -RunPath $runtime -PythonPath $PythonPath
    if ((Test-Path -LiteralPath (Join-Path $runtime 'stage-5-flicker-eye1.ppm')) -or
        !(Test-Path -LiteralPath (Join-Path $runtime 'stage-5-flicker-eye1.png')) -or
        !(Test-Path -LiteralPath (Join-Path $runtime 'flicker-summary.json'))) { throw 'Flicker capture cleanup lost visual evidence or retained raw captures.' }
    $runtimeReport=Get-Content -LiteralPath (Join-Path $runtime 'runtime-summary.json') -Raw | ConvertFrom-Json
    if ($runtimeReport.nativeHealthy -or $runtimeReport.independentStereoValidated -or $runtimeReport.gameplayValidated) { throw 'Missing runtime state became accepted evidence.' }
    $runtimeReceipt=Get-Content -LiteralPath (Join-Path $runtime 'capture-cleanup.json') -Raw | ConvertFrom-Json
    if ($runtimeReceipt.status -ne 'complete' -or (Test-Path -LiteralPath (Join-Path $runtime 'frame-1.raw'))) { throw 'Runtime capture finalization failed.' }
    if (!(Test-Path -LiteralPath (Join-Path $runtime 'protected-0.bin'))) { throw 'Runtime finalization removed a protected file.' }

    $userEnded=New-Fixture
    @{result='session_end_requested';runtimeSession=$true;untilExit=$true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $userEnded 'session.json')
    [IO.File]::WriteAllText((Join-Path $userEnded 'runtime-stopped'),'{"failed":true}')
    & $finish -RunPath $userEnded -PythonPath $PythonPath
    $endedReceipt=Get-Content -LiteralPath (Join-Path $userEnded 'capture-cleanup.json') -Raw | ConvertFrom-Json
    $endedReport=Get-Content -LiteralPath (Join-Path $userEnded 'runtime-summary.json') -Raw | ConvertFrom-Json
    if ($endedReceipt.status -ne 'complete' -or $endedReport.nativeHealthy -or (Test-Path -LiteralPath (Join-Path $userEnded 'frame-1.raw'))) { throw 'User-ended failure was not cleaned or became healthy.' }
    $invalidEnd=New-Fixture
    @{result='session_end_requested';runtimeSession=$true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $invalidEnd 'session.json')
    Assert-Rejected { & $finish -RunPath $invalidEnd -Action Plan } 'Unidentified user session'

    $bad=New-Fixture
    [IO.File]::WriteAllText((Join-Path $bad 'replay-summary.json'),'{ invalid summary')
    Assert-Rejected { & $finish -RunPath $bad -PythonPath $PythonPath } 'Failed analysis'
    if (!(Test-Path -LiteralPath (Join-Path $bad 'frame-1.raw'))) { throw 'Capture lost after failed analysis.' }

    $held=New-Fixture
    & $finish -RunPath $held -KeepCaptures -PythonPath $PythonPath
    if (!(Test-Path -LiteralPath (Join-Path $held 'frame-1.raw')) -or (Test-Path -LiteralPath (Join-Path $held 'capture-cleanup.json'))) { throw 'KeepCaptures failed.' }
    & $finish -RunPath $held -Action Plan -PythonPath $PythonPath
    [IO.File]::WriteAllText((Join-Path $held 'frame-1.raw'),'changed after planning')
    Assert-Rejected { & $finish -RunPath $held -Action Apply } 'Changed capture'
    if ([IO.File]::ReadAllText((Join-Path $held 'frame-1.raw')) -ne 'changed after planning') { throw 'Changed capture lost.' }

    $escape=New-Fixture
    & $finish -RunPath $escape -Action Plan -PythonPath $PythonPath
    $planPath=Join-Path $escape 'capture-cleanup-plan.json'
    $plan=Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json
    $plan.files[0].name='..\frame-1.raw'
    $plan | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $planPath
    Assert-Rejected { & $finish -RunPath $escape -Action Apply } 'Escaping manifest'
    Assert-Rejected { & $finish -RunPath (Join-Path $escape 'game') -Action Plan } 'Outside direct run scope'

    $unfinished=New-Fixture
    @{result='prepared'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $unfinished 'session.json')
    Assert-Rejected { & $finish -RunPath $unfinished -Action Plan } 'Unfinished session'
    @{result='process_exited'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $unfinished 'session.json')
    $junction=Join-Path $unfinished 'linked'
    New-Item -ItemType Junction -Path $junction -Target (Join-Path $run 'game') | Out-Null
    Assert-Rejected { & $finish -RunPath $unfinished -Action Plan } 'Linked directory'
    # Remove the verified junction itself; never recurse through its target.
    if (((Get-Item -LiteralPath $junction).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) { throw 'Expected test junction.' }
    Remove-Item -LiteralPath $junction
    Write-Output 'Capture cleanup: planning, exact deletion, evidence/backups, idempotence, keep, changed hashes, path escape, unfinished sessions and links passed.'
} finally {
    foreach ($fixture in $fixtures) {
        $full=[IO.Path]::GetFullPath($fixture)
        $legacy=[IO.Path]::GetDirectoryName($full) -eq $artifacts -and [IO.Path]::GetFileName($full) -like 'diagnostic-20990101-*'
        $grouped=[IO.Path]::GetDirectoryName($full) -eq (Join-Path $artifacts 'logs') -and [IO.Path]::GetFileName($full) -like '20990101-*'
        if (!$legacy -and !$grouped) { throw 'Test cleanup target escaped fixture scope.' }
        # On a failed link test retain its tiny fixture for inspection.
        $linked=@(Get-ChildItem -LiteralPath $full -Force | Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 })
        if (!$linked.Count) { Remove-Item -LiteralPath $full -Recurse }
    }
}

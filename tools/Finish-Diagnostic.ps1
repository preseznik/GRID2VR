param(
    [Parameter(Mandatory)][string]$RunPath,
    [ValidateSet('Plan','Apply','Complete')][string]$Action='Complete',
    [switch]$KeepCaptures,
    [string]$PythonPath='',
    [string]$InstallationRoot=''
)
$ErrorActionPreference='Stop'
$root=if($InstallationRoot){[IO.Path]::GetFullPath($InstallationRoot)}else{Split-Path $PSScriptRoot -Parent}
. (Join-Path $PSScriptRoot 'Paths.ps1')
$artifacts=[IO.Path]::GetFullPath((Get-Grid2DataRoot $root))
$run=[IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($RunPath)).TrimEnd('\')
$capturePattern='^(frame-\d+|stage-\d+[\w-]*|work-\d+[\w-]*)\.(raw|ppm|texture|surface|buffer)$'
function Assert-Unlinked([string]$path) {
    if (((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Linked cleanup path retained: $path"
    }
}
function Assert-Idle {
    if ((Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) -or
        (Test-Path -LiteralPath (Join-Path $artifacts 'diagnostic-pending.json'))) {
        throw 'A game or pending diagnostic recovery exists; captures retained.'
    }
}
function Save-Json($value,[string]$path) {
    $temporary=$path+'.tmp'
    $value | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $temporary -Encoding utf8
    Move-Item -LiteralPath $temporary -Destination $path -Force
}
function Retire-ExpandedSnapshots {
    if(!(Test-Path -LiteralPath (Join-Path $run 'cpu-snapshots.bin'))) { return }
    $snapshotPython=$PythonPath
    if(!$snapshotPython) {
        $snapshotPython=Get-Grid2Python $root
    }
    & $snapshotPython (Join-Path $PSScriptRoot 'unpack_snapshots.py') $run --retire-expanded
    if($LASTEXITCODE) { throw 'Expanded snapshot cleanup failed; packed archive and remaining files retained.' }
}
if ([IO.Path]::GetDirectoryName($run) -ne $artifacts -or
    [IO.Path]::GetFileName($run) -cnotmatch '^diagnostic-\d{8}-\d{6}-\d{3}$') {
    throw 'Cleanup is restricted to direct diagnostic timestamp folders in this workspace artifacts directory.'
}
Assert-Unlinked $root
Assert-Unlinked $artifacts
Assert-Unlinked $run
Assert-Idle
# Refuse links even when they would otherwise be retained. Never recurse here.
foreach ($entry in Get-ChildItem -LiteralPath $run -Force) { Assert-Unlinked $entry.FullName }
$sessionPath=Join-Path $run 'session.json'
$deploymentPath=Join-Path $run 'deployment-restored.json'
$session=Get-Content -LiteralPath $sessionPath -Raw | ConvertFrom-Json
$deployment=Get-Content -LiteralPath $deploymentPath -Raw | ConvertFrom-Json
$userEnded=$session.result -eq 'session_end_requested' -and $session.untilExit -eq $true -and $session.runtimeSession -eq $true
$recovered=$false
if($session.result -eq 'interrupted_session_recovered') {
    $original=Join-Path $run 'session-before-recovery.json'
    $recovered=(Test-Path -LiteralPath $original) -and $session.recovery.processExitKnown -eq $false -and
        $session.recovery.originalReceiptSha256 -eq (Get-FileHash -LiteralPath $original).Hash
}
if ((!$userEnded -and !$recovered -and $session.result -notin @('process_exited','probe_window_completed','timeout_owned_game_terminated','no_game_process')) -or
    [IO.Path]::GetFullPath($deployment.run).TrimEnd('\') -ne $run) {
    throw 'Missing completed session/recovery identity; captures retained.'
}
foreach ($name in @('d3d11.dll','GRID2VR.diagnostic-session')) {
    if (Test-Path -LiteralPath (Join-Path $deployment.gameRoot $name)) { throw "Deployment still present: $name" }
}
$receiptPath=Join-Path $run 'capture-cleanup.json'
$planPath=Join-Path $run 'capture-cleanup-plan.json'
$evidencePath=Join-Path $run 'retained-evidence.json'
$candidates=@(Get-ChildItem -LiteralPath $run -Force -File | Where-Object { $_.Name -cmatch $capturePattern })
if (Test-Path -LiteralPath $receiptPath) {
    $previous=Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ($previous.status -eq 'complete') {
        if ($candidates.Count) { throw 'New captures appeared in an already cleaned run; retained for review.' }
        Retire-ExpandedSnapshots
        Write-Output "Already cleaned: $run"
        return
    }
    if ($Action -ne 'Apply') { throw 'Interrupted cleanup: use -Action Apply with its existing manifest.' }
}
if ($Action -ne 'Apply') {
    if (Test-Path -LiteralPath $planPath) {
        if ($Action -eq 'Plan') { Write-Output "Existing bounded cleanup plan: $planPath"; return }
    } else {
        if (!$PythonPath) {
            $PythonPath=Get-Grid2Python $root
        }
        & $PythonPath (Join-Path $PSScriptRoot 'summarize_diagnostic.py') $run
        if ($LASTEXITCODE -ne 0) { throw 'Analysis failed; captures retained.' }
        if ($KeepCaptures) { Write-Output "Captures retained for inspection; finalize with tools\Finish-Diagnostic.ps1 -RunPath '$run'"; return }
        $files=@(foreach ($file in $candidates) {
            @{name=$file.Name;bytes=[long]$file.Length;modifiedUtcTicks=$file.LastWriteTimeUtc.Ticks;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
        })
        $plan=@{schema=1;run=$run;createdUtc=[DateTime]::UtcNow.ToString('o');files=$files;
            totalBytes=[long](($files | Measure-Object bytes -Sum).Sum);
            evidenceSha256=(Get-FileHash -LiteralPath $evidencePath).Hash;
            sessionSha256=(Get-FileHash -LiteralPath $sessionPath).Hash;
            deploymentSha256=(Get-FileHash -LiteralPath $deploymentPath).Hash}
        Save-Json $plan $planPath
        Write-Output "Cleanup plan: $($files.Count) captures, $([math]::Round($plan.totalBytes/1GB,3)) GiB; $planPath"
    }
    if ($Action -eq 'Plan') { return }
}
if ($KeepCaptures) { throw 'KeepCaptures cannot apply a cleanup manifest.' }
$plan=Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json
if ($plan.schema -ne 1 -or $plan.run -ne $run) { throw 'Cleanup manifest identity mismatch.' }
foreach ($pair in @(@($evidencePath,$plan.evidenceSha256),@($sessionPath,$plan.sessionSha256),@($deploymentPath,$plan.deploymentSha256))) {
    if ((Get-FileHash -LiteralPath $pair[0]).Hash -ne $pair[1]) { throw 'Cleanup evidence/receipt changed; captures retained.' }
}
$names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in $plan.files) {
    $target=[IO.Path]::GetFullPath((Join-Path $run $file.name))
    if ($file.name -cnotmatch $capturePattern -or [IO.Path]::GetDirectoryName($target) -ne $run -or !$names.Add($file.name)) {
        throw 'Unapproved, duplicate or escaping cleanup target.'
    }
}
Assert-Idle
$removed=[Collections.Generic.List[object]]::new()
$missing=[Collections.Generic.List[string]]::new()
$receipt=@{schema=1;run=$run;status='in_progress';startedUtc=[DateTime]::UtcNow.ToString('o');manifestSha256=(Get-FileHash -LiteralPath $planPath).Hash}
Save-Json $receipt $receiptPath
try {
    foreach ($file in $plan.files) {
        $target=Join-Path $run $file.name
        if (!(Test-Path -LiteralPath $target)) { $missing.Add($file.name); continue }
        Assert-Unlinked $run
        Assert-Unlinked $target
        $item=Get-Item -LiteralPath $target
        if ($item.Length -ne $file.bytes -or $item.LastWriteTimeUtc.Ticks -ne $file.modifiedUtcTicks -or
            (Get-FileHash -LiteralPath $target).Hash -ne $file.sha256) { throw "Changed capture retained: $target" }
        Remove-Item -LiteralPath $target
        $removed.Add(@{name=$file.name;bytes=$file.bytes})
    }
    $receipt.status='complete'
} catch {
    $receipt.error=$_.Exception.Message
    throw
} finally {
    $receipt.finishedUtc=[DateTime]::UtcNow.ToString('o')
    $receipt.removed=@($removed.ToArray()); $receipt.alreadyMissing=@($missing.ToArray())
    $receipt.removedBytes=[long](($removed | Measure-Object bytes -Sum).Sum)
    Save-Json $receipt $receiptPath
}
Write-Output "Cleaned $($removed.Count) captures, $([math]::Round($receipt.removedBytes/1GB,3)) GiB; retained logs, summaries, snapshots and backups."
Retire-ExpandedSnapshots

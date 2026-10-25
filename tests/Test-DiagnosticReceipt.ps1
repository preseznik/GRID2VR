$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path $root ('build/receipt-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    $run=Join-Path $fixture 'run'; $documents=Join-Path $fixture 'documents'
    New-Item -ItemType Directory -Path $run,(Join-Path $documents 'benchmarks') | Out-Null
    $receipt=@{result='process_exited';startedUtc=[DateTime]::UtcNow.AddMinutes(-1).ToString('o');exits=@(@{id=123;code=0});documentResults=@(@{before='before';after='after'})}
    # A malformed game report used to discard the exit and restoration hashes.
    [IO.File]::WriteAllText((Join-Path $documents 'benchmarks/bad.xml'),'<broken')
    & (Join-Path $root 'tools/Write-DiagnosticReceipt.ps1') -Receipt $receipt -Run $run -Documents $documents
    $saved=Get-Content -LiteralPath (Join-Path $run 'session.json') -Raw | ConvertFrom-Json
    if(!$saved.benchmarkReportError -or $saved.exits[0].code -ne 0 -or $saved.documentResults[0].after -ne 'after') { throw 'Report failure lost recovery evidence.' }
    # Reproduce the actual 0.1.0 packaging omission with the helper absent.
    Copy-Item -LiteralPath (Join-Path $root 'tools/Write-DiagnosticReceipt.ps1') -Destination $fixture
    & (Join-Path $fixture 'Write-DiagnosticReceipt.ps1') -Receipt $receipt -Run $run -Documents $documents
    $saved=Get-Content -LiteralPath (Join-Path $run 'session.json') -Raw | ConvertFrom-Json
    if(!$saved.benchmarkReportError -or $saved.result -ne 'process_exited') { throw 'Missing helper lost final receipt.' }
    . (Join-Path $root 'tools/User-Session.ps1')
    $game=Join-Path $fixture 'game'; New-Item -ItemType Directory -Path $game | Out-Null
    Write-SessionJson (Join-Path $run 'session.json') @{result='prepared';exits=@()}
    Write-SessionJson (Join-Path $run 'deployment-restored.json') @{run=$run;gameRoot=$game}
    $original=(Get-FileHash -LiteralPath (Join-Path $run 'session.json')).Hash
    Complete-InterruptedSessionReceipt $run $game
    $saved=Read-SessionJson (Join-Path $run 'session.json')
    if($saved.result -ne 'interrupted_session_recovered' -or $saved.recovery.processExitKnown -ne $false -or
       $saved.recovery.originalReceiptSha256 -ne $original -or @($saved.exits).Count) { throw 'Recovery invented a process exit.' }
    Complete-InterruptedSessionReceipt $run $game
    Write-Output 'Optional report failures preserve receipts; interrupted recovery preserves unknown exit status.'
} finally {
    # This unique fixture is a direct child of the workspace build directory.
    $resolved=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($resolved) -ne (Join-Path $root 'build') -or [IO.Path]::GetFileName($resolved) -notmatch '^receipt-test-[0-9a-f]{32}$') { throw 'Invalid fixture cleanup path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

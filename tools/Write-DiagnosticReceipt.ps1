param([Parameter(Mandatory)][System.Collections.IDictionary]$Receipt,
    [Parameter(Mandatory)][string]$Run,[Parameter(Mandatory)][string]$Documents)
$ErrorActionPreference='Stop'
# Optional benchmark reporting must never discard exit/document hashes needed
# by the outer settings-restoration transaction.
try {
    $Receipt.benchmarkReports=@(& (Join-Path $PSScriptRoot 'Save-BenchmarkReports.ps1') -Documents $Documents -Run $Run -StartedUtc $Receipt.startedUtc)
} catch {
    $Receipt.benchmarkReportError=$_.Exception.Message
    Write-Warning "Benchmark report retained for inspection: $($_.Exception.Message)"
} finally {
    $target=Join-Path $Run 'session.json'
    $temporary=$target+'.tmp'
    $Receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $temporary -Encoding utf8
    Move-Item -LiteralPath $temporary -Destination $target -Force
}

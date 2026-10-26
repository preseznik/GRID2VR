param([Parameter(Mandatory)][string]$ModRoot)
$ErrorActionPreference='Stop'
$workspace=Split-Path $PSScriptRoot -Parent
$expected=Join-Path $workspace 'build/distribution-verification/game with spaces/GRID2VR'
if([IO.Path]::GetFullPath($ModRoot) -ne $expected) { throw 'This test only operates on the isolated distribution fixture.' }
. (Join-Path $ModRoot 'tools/Paths.ps1')
. (Join-Path $ModRoot 'tools/Practice-Flow.ps1')
. (Join-Path $ModRoot 'tools/Cockpit-Assets.ps1')
$game=Get-Grid2GameRoot $ModRoot
$data=Get-Grid2DataRoot $ModRoot
$run=Join-Path $data ('diagnostic-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$journal=Join-Path $data 'diagnostic-pending.json'
if(Test-Path -LiteralPath $journal) { throw 'Existing recovery retained.' }
$proxy=Join-Path $game 'd3d11.dll'
if(Test-Path -LiteralPath $proxy) { throw 'Existing proxy retained.' }
Copy-Item -LiteralPath (Join-Path $data 'practice-flow-20261014/flow.bin') -Destination (Join-Path $run 'practice-flow.bin')
$flow=New-PracticeFlowDeployment $game $run
$camera=@(Get-CockpitAssetSelection $ModRoot $game HeadOnly s20 $false $true)
New-CockpitAssetsDeployment $game $run $ModRoot $camera
Copy-Item -LiteralPath (Get-Grid2Proxy $ModRoot) -Destination $proxy
$hash=(Get-FileHash -LiteralPath $proxy).Hash
@{gameRoot=$game;run=$run;files=@(@{name='d3d11.dll';sha256=$hash});practiceFlow=$flow;cockpitAssets=$camera} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $journal
try {
    Install-PracticeFlow $game $run $flow
    Install-CockpitAssets $game $run $camera
} finally {
    & (Join-Path $ModRoot 'tools/Run-Diagnostic.ps1') -Action Restore
}
if((Test-Path -LiteralPath $proxy) -or (Test-Path -LiteralPath $journal)) { throw 'Installed recovery left owned files.' }
if((Get-FileHash -LiteralPath (Join-Path $game 'system/flow.bin')).Hash -ne $flow.originalSha256) { throw 'Practice flow was not restored.' }
foreach($entry in $camera) { if((Get-FileHash -LiteralPath (Join-Path $game $entry.name)).Hash -ne $entry.originalSha256) { throw 'Camera was not restored.' } }
# Exercise the shipped summary/cleanup path with a tiny synthetic readback.
@{result='prepared';runtimeSession=$true;untilExit=$true;processes=@();exits=@();terminatedOwnedProcessIds=@()} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'session.json')
. (Join-Path $ModRoot 'tools/User-Session.ps1')
Complete-InterruptedSessionReceipt $run $game
[IO.File]::WriteAllBytes((Join-Path $run 'frame-00001.raw'),[byte[]](1,2,3,4))
& (Join-Path $ModRoot 'tools/Finish-Diagnostic.ps1') -RunPath $run
if(Test-Path -LiteralPath (Join-Path $run 'frame-00001.raw')) { throw 'Capture cleanup failed.' }
if(!(Test-Path -LiteralPath (Join-Path $run 'retained-evidence.json'))) { throw 'Analysis receipt missing.' }
# Exercise report collection from the shipped support tree (omitted in 0.1.0).
$documents=Join-Path $run 'documents'
New-Item -ItemType Directory -Path (Join-Path $documents 'benchmarks') | Out-Null
[IO.File]::WriteAllText((Join-Path $documents 'benchmarks/test.xml'),'<benchmark><totals races="1" race_time="60"/></benchmark>')
$reportRun=Join-Path $run 'report-check'; New-Item -ItemType Directory -Path $reportRun | Out-Null
& (Join-Path $ModRoot 'tools/Write-DiagnosticReceipt.ps1') -Receipt @{startedUtc=[DateTime]::UtcNow.AddMinutes(-1).ToString('o');result='process_exited'} -Run $reportRun -Documents $documents
$report=Get-Content -LiteralPath (Join-Path $reportRun 'session.json') -Raw | ConvertFrom-Json
if($report.benchmarkReportError -or @($report.benchmarkReports).Count -ne 1) { throw 'Installed benchmark report helper failed.' }
Write-Output 'Installed flow/cockpit/proxy recovery and bounded capture cleanup passed; no game launched.'

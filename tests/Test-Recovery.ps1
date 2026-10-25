$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path $root ('artifacts\recovery-test-' + [guid]::NewGuid().ToString('N'))
$game=Join-Path $fixture 'game'
$run=Join-Path $fixture 'run'
New-Item -ItemType Directory -Path $game,$run -Force | Out-Null
$journal=Join-Path $fixture 'pending.json'
$proxy=Join-Path $game 'd3d11.dll'
$unrelated=Join-Path $game 'user-file.txt'
[IO.File]::WriteAllText($proxy,'owned diagnostic fixture')
[IO.File]::WriteAllText($unrelated,'preserve this')
$hash=(Get-FileHash -LiteralPath $proxy).Hash
function Write-Journal([string]$name,[string]$expected) {
    @{gameRoot=$game;run=$run;files=@(@{name=$name;sha256=$expected})} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $journal
}
function Assert-Rejected([string]$label) {
    $rejected=$false
    try { & (Join-Path $root 'tools\Run-Diagnostic.ps1') -Action Restore -GameRoot $game -JournalPath $journal } catch {$rejected=$true}
    if(!$rejected){throw "$label was not rejected"}
}
Write-Journal 'd3d11.dll' ('0'*64)
Assert-Rejected 'Foreign proxy hash'
if(!(Test-Path -LiteralPath $proxy)){throw 'Foreign proxy was deleted'}
Write-Journal '..\user-file.txt' $hash
Assert-Rejected 'Escaping target'
# A practice recovery error must retain proxy ownership and its journal. A
# different asset must never become an authorized target via journal metadata.
@{gameRoot=$game;run=$run;files=@(@{name='d3d11.dll';sha256=$hash});practiceFlow=@{
    name='system/other.bin';originalSha256=('0'*64);sha256=('1'*64)
}} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $journal
Assert-Rejected 'Foreign practice journal entry'
if(!(Test-Path -LiteralPath $proxy) -or !(Test-Path -LiteralPath $journal)){throw 'Practice rejection lost proxy ownership'}
@{gameRoot=$game;run=$run;files=@(@{name='d3d11.dll';sha256=$hash});practiceFlow=@{
    name='system/flow.bin'
    originalSha256='8b2f7fce029ed2fe2e82d837d0e180f14347e14737f71566b0a318c46bd32894'
    sha256='f2dc0085047a3cf29feac15e62002d9866c71261a7aa325ebb27571ed8b07d24'
}} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $journal
Assert-Rejected 'Missing practice backup'
if(!(Test-Path -LiteralPath $proxy) -or !(Test-Path -LiteralPath $journal)){throw 'Missing practice backup lost proxy ownership'}
Write-Journal 'd3d11.dll' $hash
@{gameRoot=$game;run=$run;files=@(@{name='d3d11.dll';sha256=$hash});cockpitCamera=@{
    name='cars/models/pag/cameras.xml';car='pag';mode='HeadOnly';originalSha256=('0'*64);sha256=('1'*64)
}} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $journal
Assert-Rejected 'Foreign cockpit journal entry'
if(!(Test-Path -LiteralPath $proxy) -or !(Test-Path -LiteralPath $journal)){throw 'Cockpit rejection lost proxy ownership'}
@{gameRoot=$game;run=$run;files=@(@{name='d3d11.dll';sha256=$hash});cockpitAssets=@(@{
    name='cars/models/pag/cameras.xml';car='pag';mode='HeadOnly';originalSha256=('0'*64);sha256=('1'*64)
})} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $journal
Assert-Rejected 'Foreign multi-car cockpit journal entry'
if(!(Test-Path -LiteralPath $proxy) -or !(Test-Path -LiteralPath $journal)){throw 'Multi-car cockpit rejection lost proxy ownership'}
Write-Journal 'd3d11.dll' $hash
& (Join-Path $root 'tools\Run-Diagnostic.ps1') -Action Restore -GameRoot $game -JournalPath $journal
if((Test-Path -LiteralPath $proxy) -or (Test-Path -LiteralPath $journal)){throw 'Owned proxy recovery incomplete'}
if([IO.File]::ReadAllText($unrelated) -ne 'preserve this'){throw 'Unrelated data changed'}
if(!(Test-Path -LiteralPath (Join-Path $run 'deployment-restored.json'))){throw 'Recovery receipt missing'}
# This offset caused the first live timeout to retain its owned process.
$start=[datetimeoffset]'2026-09-28T20:16:38Z'
$created=[datetimeoffset]'2026-09-28T22:16:40+02:00'
if($created.UtcTicks -lt $start.UtcTicks){throw 'UTC ownership comparison regressed'}
Write-Output 'Recovery: foreign hash, path escape, practice refusal/ownership, owned cleanup, unrelated preservation, receipt and UTC checks passed.'

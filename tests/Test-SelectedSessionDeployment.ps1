$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path $root ('build/selection-recovery-test-'+[guid]::NewGuid().ToString('N'))
$script=Join-Path $root 'tools/Run-Diagnostic.ps1'
function New-Case([string]$Name) {
    $game=Join-Path $fixture "$Name/game"
    $run=Join-Path $fixture "$Name/run"
    New-Item -ItemType Directory -Path $game,$run | Out-Null
    $entries=@()
    foreach($file in @('d3d11.dll','GRID2VR.diagnostic-session','GRID2VR.session.xml')) {
        $target=Join-Path $game $file
        [IO.File]::WriteAllText($target,'owned fixture '+$file)
        $entries+=@{name=$file;sha256=(Get-FileHash -LiteralPath $target).Hash}
    }
    [IO.File]::WriteAllText((Join-Path $game 'example_benchmark_attract.xml'),'original default fixture')
    $journal=Join-Path $fixture "$Name/pending.json"
    @{gameRoot=$game;run=$run;files=$entries} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $journal
    return @{game=$game;run=$run;journal=$journal}
}
function Reject([scriptblock]$Action) {
    $before=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    if(!$rejected) { throw 'Changed or foreign session configuration was accepted.' }
    $after=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    if(Compare-Object $before $after) { throw 'Rejected recovery changed files.' }
}
try {
    foreach($phase in @('installed','partial','old-journal')) {
        $case=New-Case $phase
        if($phase -ne 'installed') { Remove-Item -LiteralPath (Join-Path $case.game 'GRID2VR.session.xml') }
        if($phase -eq 'old-journal') {
            $journal=Get-Content -LiteralPath $case.journal -Raw | ConvertFrom-Json
            $journal.files=@($journal.files | Where-Object name -ne 'GRID2VR.session.xml')
            $journal | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $case.journal
        }
        & $script -Action Restore -GameRoot $case.game -JournalPath $case.journal | Out-Null
        & $script -Action Restore -GameRoot $case.game -JournalPath $case.journal | Out-Null
        if(Test-Path -LiteralPath $case.journal) { throw 'Recovery journal not finalized.' }
        foreach($file in @('d3d11.dll','GRID2VR.diagnostic-session','GRID2VR.session.xml')) {
            if(Test-Path -LiteralPath (Join-Path $case.game $file)) { throw 'Owned deployment left behind.' }
        }
        if([IO.File]::ReadAllText((Join-Path $case.game 'example_benchmark_attract.xml')) -ne 'original default fixture') { throw 'Shipped default changed.' }
    }
    $case=New-Case 'changed'
    [IO.File]::WriteAllText((Join-Path $case.game 'GRID2VR.session.xml'),'foreign replacement')
    Reject { & $script -Action Restore -GameRoot $case.game -JournalPath $case.journal }
    $case=New-Case 'foreign-target'
    $journal=Get-Content -LiteralPath $case.journal -Raw | ConvertFrom-Json
    $journal.files[2].name='example_benchmark_attract.xml'
    $journal | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $case.journal
    Reject { & $script -Action Restore -GameRoot $case.game -JournalPath $case.journal }
    Write-Output 'Selected-session recovery passed: exact XML ownership, interrupted/legacy deployment, idempotence, changed-file refusal and untouched shipped defaults.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    $parent=[IO.Path]::GetFullPath((Join-Path $root 'build'))
    if([IO.Path]::GetDirectoryName($resolved) -ne $parent -or [IO.Path]::GetFileName($resolved) -notmatch '^selection-recovery-test-[0-9a-f]{32}$') { throw 'Fixture escaped build.' }
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\Practice-Flow.ps1')
$fixture=Join-Path $root ('artifacts\practice-recovery-test-'+[guid]::NewGuid().ToString('N'))
$original=[Text.Encoding]::UTF8.GetBytes('original flow fixture')
$modified=[Text.Encoding]::UTF8.GetBytes('practice flow fixture')
$states=[Text.Encoding]::UTF8.GetBytes('states fixture')
function Hash-Bytes([byte[]]$Bytes) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
}
# The production helper still enforces its fingerprints; use synthetic bytes in
# this isolated test rather than distributing a proprietary game asset fixture.
$script:PracticeOriginalHash=Hash-Bytes $original
$script:PracticeModifiedHash=Hash-Bytes $modified
$script:PracticeStatesHash=Hash-Bytes $states
function New-Case([string]$Name) {
    $game=Join-Path $fixture "$Name\game"
    $run=Join-Path $fixture "$Name\run"
    New-Item -ItemType Directory -Path (Join-Path $game 'system'),$run | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $game 'system\flow.bin'),$original)
    [IO.File]::WriteAllBytes((Join-Path $game 'system\states.bin'),$states)
    [IO.File]::WriteAllBytes((Join-Path $run 'practice-flow.bin'),$modified)
    return @{game=$game;run=$run;paths=(Get-PracticeFlowPaths $game $run)}
}
function Assert-Rejected([scriptblock]$Action,[string]$Label) {
    $before=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    $rejected=$false
    try { & $Action } catch { $rejected=$true }
    if(!$rejected) { throw "$Label was accepted" }
    $after=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    if(Compare-Object $before $after) { throw "$Label mutated a fixture before rejection" }
}
try {
    foreach($phase in @('before-install','after-install','install-temp','restore-temp')) {
        $case=New-Case $phase
        $entry=New-PracticeFlowDeployment $case.game $case.run
        if($phase -in @('after-install','restore-temp')) {
            Install-PracticeFlow $case.game $case.run $entry
            Assert-PracticeHash $case.paths.target $script:PracticeModifiedHash
        }
        if($phase -eq 'install-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$modified) }
        if($phase -eq 'restore-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$original) }
        Restore-PracticeFlow $case.game $case.run $entry
        Restore-PracticeFlow $case.game $case.run $entry
        Assert-PracticeHash $case.paths.target $script:PracticeOriginalHash
        Assert-PracticeHash $case.paths.backup $script:PracticeOriginalHash
        if(Test-Path -LiteralPath $case.paths.temporary) { throw "Temporary file retained in $phase" }
    }
    foreach($changed in @('target','backup','temporary')) {
        $case=New-Case ('foreign-'+$changed)
        $entry=New-PracticeFlowDeployment $case.game $case.run
        Install-PracticeFlow $case.game $case.run $entry
        [IO.File]::WriteAllText($case.paths[$changed],'foreign or incomplete data')
        Assert-Rejected { Restore-PracticeFlow $case.game $case.run $entry } "Foreign $changed"
    }
    $case=New-Case 'missing-backup'
    $entry=New-PracticeFlowDeployment $case.game $case.run
    Remove-Item -LiteralPath $case.paths.backup
    Assert-Rejected { Restore-PracticeFlow $case.game $case.run $entry } 'Missing backup'
    $case=New-Case 'entry-guards'
    $entry=New-PracticeFlowDeployment $case.game $case.run
    $entry.name='../foreign.bin'
    Assert-Rejected { Restore-PracticeFlow $case.game $case.run $entry } 'Escaping entry'
    $entry.name='system/flow.bin'; $entry.sha256='0'*64
    Assert-Rejected { Restore-PracticeFlow $case.game $case.run $entry } 'Forged entry hash'
    foreach($changed in @('target','source','states')) {
        $case=New-Case ('preinstall-'+$changed)
        $entry=New-PracticeFlowDeployment $case.game $case.run
        [IO.File]::WriteAllText($case.paths[$changed],'changed after preparation')
        Assert-Rejected { Install-PracticeFlow $case.game $case.run $entry } "Changed $changed before install"
    }
    Write-Output 'Practice recovery: atomic replacement, interrupted phases, idempotence, changed/missing backups and foreign-file guards passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    $parent=[IO.Path]::GetFullPath((Join-Path $root 'artifacts'))
    if([IO.Path]::GetDirectoryName($resolved) -ne $parent -or [IO.Path]::GetFileName($resolved) -notmatch '^practice-recovery-test-[0-9a-f]{32}$') { throw 'Fixture cleanup escaped artifacts.' }
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

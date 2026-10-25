$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\Practice-Flow.ps1')
. (Join-Path $root 'tools\Instance-Capacity.ps1')
$fixture=Join-Path $root ('artifacts\capacity-recovery-test-'+[guid]::NewGuid().ToString('N'))
$original=[Text.Encoding]::UTF8.GetBytes('original flow fixture')
$modified=[Text.Encoding]::UTF8.GetBytes('capacity flow fixture')
function Hash-Bytes([byte[]]$Bytes) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
}
# The production helper still enforces its fingerprints; use synthetic bytes in
# this isolated test rather than distributing a proprietary game asset fixture.
$script:CapacityOriginalHash=Hash-Bytes $original
$script:CapacityModifiedHash=Hash-Bytes $modified
function New-Case([string]$Name) {
    $game=Join-Path $fixture "$Name\game"
    $run=Join-Path $fixture "$Name\run"
    New-Item -ItemType Directory -Path (Join-Path $game 'tracks/generic'),(Join-Path $run 'instance-capacity-prepared') | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $game 'tracks\generic\ornament_system_settings.xml'),$original)
    [IO.File]::WriteAllBytes((Join-Path $run 'instance-capacity-prepared\ornament-system-capacity.bin'),$modified)
    return @{game=$game;run=$run;paths=(Get-InstanceCapacityPaths $game $run)}
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
        $entry=New-InstanceCapacityDeployment $case.game $case.run
        if($phase -in @('after-install','restore-temp')) {
            Install-InstanceCapacity $case.game $case.run $entry
            Assert-PracticeHash $case.paths.target $script:CapacityModifiedHash
        }
        if($phase -eq 'install-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$modified) }
        if($phase -eq 'restore-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$original) }
        Restore-InstanceCapacity $case.game $case.run $entry
        Restore-InstanceCapacity $case.game $case.run $entry
        Assert-PracticeHash $case.paths.target $script:CapacityOriginalHash
        Assert-PracticeHash $case.paths.backup $script:CapacityOriginalHash
        if(Test-Path -LiteralPath $case.paths.temporary) { throw "Temporary file retained in $phase" }
    }
    foreach($changed in @('target','backup','temporary')) {
        $case=New-Case ('foreign-'+$changed)
        $entry=New-InstanceCapacityDeployment $case.game $case.run
        Install-InstanceCapacity $case.game $case.run $entry
        [IO.File]::WriteAllText($case.paths[$changed],'foreign or incomplete data')
        Assert-Rejected { Restore-InstanceCapacity $case.game $case.run $entry } "Foreign $changed"
    }
    $case=New-Case 'missing-backup'
    $entry=New-InstanceCapacityDeployment $case.game $case.run
    Remove-Item -LiteralPath $case.paths.backup
    Assert-Rejected { Restore-InstanceCapacity $case.game $case.run $entry } 'Missing backup'
    $case=New-Case 'entry-guards'
    $entry=New-InstanceCapacityDeployment $case.game $case.run
    $entry.name='../foreign.bin'
    Assert-Rejected { Restore-InstanceCapacity $case.game $case.run $entry } 'Escaping entry'
    $entry.name='tracks/generic/ornament_system_settings.xml'; $entry.sha256='0'*64
    Assert-Rejected { Restore-InstanceCapacity $case.game $case.run $entry } 'Forged entry hash'
    foreach($changed in @('target','source')) {
        $case=New-Case ('preinstall-'+$changed)
        $entry=New-InstanceCapacityDeployment $case.game $case.run
        [IO.File]::WriteAllText($case.paths[$changed],'changed after preparation')
        Assert-Rejected { Install-InstanceCapacity $case.game $case.run $entry } "Changed $changed before install"
    }
    Write-Output 'InstanceCapacity recovery: atomic replacement, interrupted phases, idempotence, changed/missing backups and foreign-file guards passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    $parent=[IO.Path]::GetFullPath((Join-Path $root 'artifacts'))
    if([IO.Path]::GetDirectoryName($resolved) -ne $parent -or [IO.Path]::GetFileName($resolved) -notmatch '^capacity-recovery-test-[0-9a-f]{32}$') { throw 'Fixture cleanup escaped artifacts.' }
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

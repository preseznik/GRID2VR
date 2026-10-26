$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\Cinema-Matte.ps1')
$fixture=Join-Path $root ('artifacts\cinema-recovery-test-'+[guid]::NewGuid().ToString('N'))
$original=[byte[]]::new(724829)
$original[0]=0x51;$original[3000]=0x71;$original[724828]=0xB2
[byte[]]$indices=0,0,0,1,0,2,0,3,0,2,0,1
foreach($offset in @(456040,457099)) { [Array]::Copy($indices,0,$original,$offset,12) }
$positions=@{
    514752='c220000040c00000348ccde2c2200000c0be8cacb48ccde24220000040c00000348ccde242200000c0be8cacb48ccde2'
    516161='c220000040c00000348ccde2c2200000c0c00000b48ccde24220000040c00000348ccde242200000c0c00000b48ccde2'
}
foreach($offset in $positions.Keys) {
    for($i=0;$i -lt 48;$i++) { $original[$offset+$i]=[Convert]::ToByte($positions[$offset].Substring($i*2,2),16) }
}
$replayY=@{
    120095=@('3e0783c0bea9fcb8ba82b0003a2e4000','3e0783c0c28022ebbe45064b3e035987')
    120131=@('be4cccd0000000003ea9fcb8bea9fcb8','c280000000000000428022ebc28022eb')
}
foreach($offset in $replayY.Keys) {
    for($i=0;$i -lt 16;$i++) { $original[$offset+$i]=[Convert]::ToByte($replayY[$offset][0].Substring($i*2,2),16) }
}
$modified=[byte[]]$original.Clone()
foreach($offset in @(514752,516161)) { [Array]::Clear($modified,$offset,48) }
$verticesOnly=[byte[]]$modified.Clone()
foreach($offset in $replayY.Keys) {
    for($i=0;$i -lt 16;$i++) { $modified[$offset+$i]=[Convert]::ToByte($replayY[$offset][1].Substring($i*2,2),16) }
}
$indexOnly=[byte[]]$original.Clone()
foreach($offset in @(456040,457099)) { [Array]::Clear($indexOnly,$offset,12) }
$executable=[Text.Encoding]::UTF8.GetBytes('verified executable fixture')
function Hash-Bytes([byte[]]$Bytes) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
}
function Read-BeFloat([byte[]]$Bytes,[int]$Offset) {
    [byte[]]$word=$Bytes[$Offset..($Offset+3)];[Array]::Reverse($word)
    return [double][BitConverter]::ToSingle($word,0)
}
function Read-YPolynomial([byte[]]$Bytes,[int]$Offset,[double]$T) {
    $value=0.0
    for($i=3;$i -ge 0;$i--) { $value=$value*$T+(Read-BeFloat $Bytes ($Offset+4*$i)) }
    return $value
}
# The production helper still enforces its fingerprints; use synthetic bytes in
# this isolated test rather than distributing a proprietary game asset fixture.
$script:CinemaOriginalHash=Hash-Bytes $original
$script:CinemaModifiedHash=Hash-Bytes $modified
$script:CinemaIndexOnlyHash=Hash-Bytes $indexOnly
$script:CinemaVerticesOnlyHash=Hash-Bytes $verticesOnly
$script:CinemaExecutableHash=Hash-Bytes $executable
# Fixture-only executable validator; preserves the transaction's changed-file checks.
function Get-Grid2ExecutableHash([string]$Path,[string]$Variant='Avx') {
    $actual=(Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash.ToLowerInvariant()
    if($actual -ne $script:CinemaExecutableHash) { throw 'Changed fixture executable.' }
    return $actual
}

function New-Case([string]$Name) {
    $game=Join-Path $fixture "$Name\game"
    $run=Join-Path $fixture "$Name\run"
    New-Item -ItemType Directory -Path (Join-Path $game 'frontend\databases'),$run | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $game 'frontend\databases\overlayPersistent.pssg'),$original)
    [IO.File]::WriteAllBytes((Join-Path $game 'grid2_avx.exe'),$executable)
    return @{game=$game;run=$run;paths=(Get-CinemaMattePaths $game $run)}
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
    $case=New-Case 'preflight'
    Assert-CinemaMatteOriginal $case.game
    Assert-Rejected { New-CinemaMatteDeployment $case.game $case.game } 'Backup inside installation'
    [IO.File]::WriteAllText($case.paths.target,'foreign asset')
    Assert-Rejected { Assert-CinemaMatteOriginal $case.game } 'Foreign preflight asset'
    foreach($phase in @('before-install','after-install','install-temp','restore-temp')) {
        $case=New-Case $phase
        $entry=New-CinemaMatteDeployment $case.game $case.run
        Assert-CinemaHash $case.paths.source $script:CinemaModifiedHash
        $generated=[IO.File]::ReadAllBytes($case.paths.source)
        foreach($offset in @(456040,457099)) {
            if([BitConverter]::ToString($generated,$offset,12) -cne [BitConverter]::ToString($indices)) { throw 'Original quad indices changed.' }
        }
        foreach($offset in @(514752,516161)) {
            if(@($generated[$offset..($offset+47)] | Where-Object { $_ -ne 0 }).Count) { throw 'Cinema quad retains area.' }
        }
        if([BitConverter]::ToString($generated,120095,4) -cne [BitConverter]::ToString($original,120095,4)) { throw 'Replay ON endpoint changed.' }
        if([BitConverter]::ToString($generated,120131,4) -cne 'C2-80-00-00') { throw 'Replay OFF endpoint was not moved below the canvas.' }
        $on=Read-BeFloat $original 120095;$off=Read-BeFloat $original 120131
        $scale=(-64.0-$on)/($off-$on);$previous=$on
        for($step=0;$step -le 100;$step++) {
            $t=$step/100.0
            $native=Read-YPolynomial $original 120095 $t
            $actual=Read-YPolynomial $generated 120095 $t
            if([Math]::Abs($actual-($on+$scale*($native-$on))) -gt 0.00002 -or $actual -gt $previous+0.00002) { throw 'Replay exit travel changed its native curve shape or reverses direction.' }
            if((Read-YPolynomial $generated 120131 $t) -gt -50.0) { throw 'Replay OFF continuation can re-enter the canvas.' }
            $previous=$actual
        }
        Assert-CinemaHash $case.paths.target $script:CinemaOriginalHash
        Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Repeated prepare preserves source and backup'
        if($phase -in @('after-install','restore-temp')) {
            Install-CinemaMatte $case.game $case.run $entry
            Assert-CinemaHash $case.paths.target $script:CinemaModifiedHash
        }
        if($phase -eq 'install-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$modified) }
        if($phase -eq 'restore-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$original) }
        Restore-CinemaMatte $case.game $case.run $entry
        Restore-CinemaMatte $case.game $case.run $entry
        Assert-CinemaHash $case.paths.target $script:CinemaOriginalHash
        Assert-CinemaHash $case.paths.backup $script:CinemaOriginalHash
        if(Test-Path -LiteralPath $case.paths.temporary) { throw "Temporary file retained in $phase" }
    }
    foreach($legacy in @(@{name='index-only';bytes=$indexOnly;hash=$script:CinemaIndexOnlyHash},@{name='vertices-only';bytes=$verticesOnly;hash=$script:CinemaVerticesOnlyHash})) {
        $case=New-Case ('superseded-'+$legacy.name)
        [IO.File]::WriteAllBytes($case.paths.backup,$original)
        [IO.File]::WriteAllBytes($case.paths.source,$legacy.bytes)
        $entry=@{name='frontend/databases/overlayPersistent.pssg';originalSha256=$script:CinemaOriginalHash;sha256=$legacy.hash}
        Assert-Rejected { Install-CinemaMatte $case.game $case.run $entry } 'Superseded candidate cannot install'
        [IO.File]::WriteAllBytes($case.paths.target,$legacy.bytes)
        [IO.File]::WriteAllBytes($case.paths.temporary,$legacy.bytes)
        Restore-CinemaMatte $case.game $case.run $entry
        Restore-CinemaMatte $case.game $case.run $entry
        Assert-CinemaHash $case.paths.target $script:CinemaOriginalHash
        if(Test-Path -LiteralPath $case.paths.temporary) { throw 'Superseded temporary file retained.' }
    }
    foreach($changed in @('target','backup','temporary')) {
        $case=New-Case ('foreign-'+$changed)
        $entry=New-CinemaMatteDeployment $case.game $case.run
        Install-CinemaMatte $case.game $case.run $entry
        [IO.File]::WriteAllText($case.paths[$changed],'foreign or incomplete data')
        Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } "Foreign $changed"
    }
    $case=New-Case 'missing-backup'
    $entry=New-CinemaMatteDeployment $case.game $case.run
    Remove-Item -LiteralPath $case.paths.backup
    Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } 'Missing backup'
    $case=New-Case 'entry-guards'
    $entry=New-CinemaMatteDeployment $case.game $case.run
    $entry.name='../foreign.bin'
    Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } 'Escaping entry'
    $entry.name='frontend/databases/overlayPersistent.pssg'; $entry.sha256='0'*64
    Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } 'Forged entry hash'
    foreach($changed in @('target','source','executable')) {
        $case=New-Case ('preinstall-'+$changed)
        $entry=New-CinemaMatteDeployment $case.game $case.run
        [IO.File]::WriteAllText($case.paths[$changed],'changed after preparation')
        Assert-Rejected { Install-CinemaMatte $case.game $case.run $entry } "Changed $changed before install"
    }
    foreach($owned in @('source','backup','temporary')) {
        $case=New-Case ('existing-'+$owned)
        [IO.File]::WriteAllText($case.paths[$owned],'foreign preexisting file')
        Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } "Existing $owned before prepare"
    }
    $case=New-Case 'wrong-index-payload'
    $bad=[byte[]]$original.Clone();$bad[456043]=7
    [IO.File]::WriteAllBytes($case.paths.target,$bad)
    $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
    try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong exact index payload' }
    finally { $script:CinemaOriginalHash=$saved }
    foreach($offset in @(514752,516161)) {
        $case=New-Case ('wrong-position-'+$offset)
        $bad=[byte[]]$original.Clone();$bad[$offset]=7
        [IO.File]::WriteAllBytes($case.paths.target,$bad)
        $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
        try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong exact position payload' }
        finally { $script:CinemaOriginalHash=$saved }
    }
    foreach($offset in @(120095,120131)) {
        $case=New-Case ('wrong-animation-'+$offset)
        $bad=[byte[]]$original.Clone();$bad[$offset]=7
        [IO.File]::WriteAllBytes($case.paths.target,$bad)
        $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
        try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong replay animation payload' }
        finally { $script:CinemaOriginalHash=$saved }
    }
    Write-Output 'Cinema matte and replay OFF state: exact geometry/animation guards, unchanged ON endpoint, atomic replacement, interrupted recovery, legacy recovery and foreign-file refusal passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    $parent=[IO.Path]::GetFullPath((Join-Path $root 'artifacts'))
    if([IO.Path]::GetDirectoryName($resolved) -ne $parent -or [IO.Path]::GetFileName($resolved) -notmatch '^cinema-recovery-test-[0-9a-f]{32}$') { throw 'Fixture cleanup escaped artifacts.' }
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

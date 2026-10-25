# The only installed asset replaced by selected human practice. Keep these
# fingerprints aligned with practice_entry.py; arbitrary journal hashes are refused.
$script:PracticeOriginalHash='8b2f7fce029ed2fe2e82d837d0e180f14347e14737f71566b0a318c46bd32894'
$script:PracticeModifiedHash='f2dc0085047a3cf29feac15e62002d9866c71261a7aa325ebb27571ed8b07d24'
$script:PracticeStatesHash='07e593b3891171a7c4caa67b0b2e0ee1a9c166fc2d4bd25814ad695c439d9adc'

function Assert-PracticePlainPath([string]$Path) {
    $current=[IO.Path]::GetFullPath($Path)
    while($current) {
        if(Test-Path -LiteralPath $current) {
            if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked practice path retained: $current" }
        }
        $current=Split-Path $current -Parent
    }
}

function Get-PracticeFlowPaths([string]$Game,[string]$Run) {
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    $runPath=[IO.Path]::GetFullPath($Run).TrimEnd('\')
    if($runPath -eq $gamePath -or $runPath.StartsWith($gamePath+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Practice recovery backup must be outside the installation.' }
    if(!(Test-Path -LiteralPath $gamePath -PathType Container) -or !(Test-Path -LiteralPath $runPath -PathType Container)) { throw 'Practice game/run directory is missing.' }
    $paths=@{
        target=(Join-Path $gamePath 'system\flow.bin')
        temporary=(Join-Path $gamePath 'system\GRID2VR.practice-flow.tmp')
        source=(Join-Path $runPath 'practice-flow.bin')
        backup=(Join-Path $runPath 'practice-flow-original.bin')
        states=(Join-Path $gamePath 'system\states.bin')
    }
    foreach($path in $paths.Values) { Assert-PracticePlainPath $path }
    return $paths
}

function Assert-PracticeFlowEntry($Entry) {
    if(!$Entry -or $Entry.name -cne 'system/flow.bin' -or $Entry.sha256 -ne $script:PracticeModifiedHash -or $Entry.originalSha256 -ne $script:PracticeOriginalHash) {
        throw 'Unsupported practice recovery entry.'
    }
}

function Assert-PracticeHash([string]$Path,[string]$Expected) {
    Assert-PracticePlainPath $Path
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-FileHash -LiteralPath $Path).Hash -ne $Expected) { throw "Changed or missing practice file retained: $Path" }
}

function New-PracticeFlowDeployment([string]$Game,[string]$Run) {
    $paths=Get-PracticeFlowPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before preparing practice deployment.' }
    if(Test-Path -LiteralPath $paths.temporary) { throw 'Existing practice temporary file retained.' }
    Assert-PracticeHash $paths.target $script:PracticeOriginalHash
    Assert-PracticeHash $paths.source $script:PracticeModifiedHash
    Assert-PracticeHash $paths.states $script:PracticeStatesHash
    [IO.File]::Copy($paths.target,$paths.backup,$false)
    Assert-PracticeHash $paths.backup $script:PracticeOriginalHash
    return @{name='system/flow.bin';originalSha256=$script:PracticeOriginalHash;sha256=$script:PracticeModifiedHash}
}

function Write-PracticeFlowAtomic($Paths,[string]$Source,[string]$Expected,[string]$Before) {
    # An interrupted copy leaves the installed asset intact. Only a complete,
    # hash-checked sibling is atomically substituted for it on the same volume.
    [IO.File]::Copy($Source,$Paths.temporary,$false)
    Assert-PracticeHash $Paths.temporary $Expected
    $file=[IO.File]::Open($Paths.temporary,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $file.Flush($true) } finally { $file.Dispose() }
    Assert-PracticeHash $Paths.target $Before
    [IO.File]::Replace($Paths.temporary,$Paths.target,[Management.Automation.Language.NullString]::Value)
    Assert-PracticeHash $Paths.target $Expected
}

function Install-PracticeFlow([string]$Game,[string]$Run,$Entry) {
    Assert-PracticeFlowEntry $Entry
    $paths=Get-PracticeFlowPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing GRID 2 process retained.' }
    Assert-PracticeHash $paths.target $script:PracticeOriginalHash
    Assert-PracticeHash $paths.backup $script:PracticeOriginalHash
    Assert-PracticeHash $paths.source $script:PracticeModifiedHash
    Assert-PracticeHash $paths.states $script:PracticeStatesHash
    Write-PracticeFlowAtomic $paths $paths.source $script:PracticeModifiedHash $script:PracticeOriginalHash
}

function Restore-PracticeFlow([string]$Game,[string]$Run,$Entry) {
    Assert-PracticeFlowEntry $Entry
    $paths=Get-PracticeFlowPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'GRID 2 must exit before restoring practice flow.' }
    Assert-PracticeHash $paths.backup $script:PracticeOriginalHash
    $current=(Get-FileHash -LiteralPath $paths.target).Hash
    if($current -notin @($script:PracticeOriginalHash,$script:PracticeModifiedHash)) { throw 'Changed installed flow retained; recovery requires inspection.' }
    # Validate everything before mutation. A partial/foreign temporary file is
    # retained for inspection; it is never taken as a valid installed asset.
    if(Test-Path -LiteralPath $paths.temporary) {
        if((Get-FileHash -LiteralPath $paths.temporary).Hash -notin @($script:PracticeOriginalHash,$script:PracticeModifiedHash)) { throw 'Changed practice temporary file retained.' }
        Remove-Item -LiteralPath $paths.temporary
    }
    if($current -eq $script:PracticeModifiedHash) {
        Write-PracticeFlowAtomic $paths $paths.backup $script:PracticeOriginalHash $script:PracticeModifiedHash
    }
    Assert-PracticeHash $paths.target $script:PracticeOriginalHash
}

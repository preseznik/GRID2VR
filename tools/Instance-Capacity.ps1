# Isolated high-profile capacity experiment. Atomic I/O/path guards are shared
# with the existing practice transaction; no practice-flow setting is changed.
$script:CapacityOriginalHash='e838736a38d9de5a83c5fa966c37805c3f9097a4ec193b0dc8602983cf8b9737'
$script:CapacityModifiedHash='e11fd1dc2c9b27a9fd5fdf4c2755999febdcdb3e20ad37bffae3f5fc60d51e3e'

function Get-InstanceCapacityPaths([string]$Game,[string]$Run) {
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    $runPath=[IO.Path]::GetFullPath($Run).TrimEnd('\')
    if($runPath -eq $gamePath -or $runPath.StartsWith($gamePath+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Capacity backup must be outside the installation.' }
    if(!(Test-Path -LiteralPath $gamePath -PathType Container) -or !(Test-Path -LiteralPath $runPath -PathType Container)) { throw 'Capacity game/run directory is missing.' }
    $paths=@{
        target=(Join-Path $gamePath 'tracks\generic\ornament_system_settings.xml')
        temporary=(Join-Path $gamePath 'tracks\generic\GRID2VR.instance-capacity.tmp')
        source=(Join-Path $runPath 'instance-capacity-prepared\ornament-system-capacity.bin')
        backup=(Join-Path $runPath 'instance-capacity-original.bin')
    }
    foreach($path in $paths.Values) { Assert-PracticePlainPath $path }
    return $paths
}
function Assert-InstanceCapacityEntry($Entry) {
    if(!$Entry -or $Entry.name -cne 'tracks/generic/ornament_system_settings.xml' -or
       $Entry.sha256 -ne $script:CapacityModifiedHash -or $Entry.originalSha256 -ne $script:CapacityOriginalHash) {
        throw 'Unsupported instance-capacity recovery entry.'
    }
}
function New-InstanceCapacityDeployment([string]$Game,[string]$Run) {
    $paths=Get-InstanceCapacityPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before preparing capacity deployment.' }
    if(Test-Path -LiteralPath $paths.temporary) { throw 'Existing capacity temporary file retained.' }
    Assert-PracticeHash $paths.target $script:CapacityOriginalHash
    Assert-PracticeHash $paths.source $script:CapacityModifiedHash
    [IO.File]::Copy($paths.target,$paths.backup,$false)
    Assert-PracticeHash $paths.backup $script:CapacityOriginalHash
    return @{name='tracks/generic/ornament_system_settings.xml';originalSha256=$script:CapacityOriginalHash;sha256=$script:CapacityModifiedHash}
}
function Install-InstanceCapacity([string]$Game,[string]$Run,$Entry) {
    Assert-InstanceCapacityEntry $Entry
    $paths=Get-InstanceCapacityPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing GRID 2 process retained.' }
    Assert-PracticeHash $paths.target $script:CapacityOriginalHash
    Assert-PracticeHash $paths.backup $script:CapacityOriginalHash
    Assert-PracticeHash $paths.source $script:CapacityModifiedHash
    Write-PracticeFlowAtomic $paths $paths.source $script:CapacityModifiedHash $script:CapacityOriginalHash
}
function Restore-InstanceCapacity([string]$Game,[string]$Run,$Entry) {
    Assert-InstanceCapacityEntry $Entry
    $paths=Get-InstanceCapacityPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'GRID 2 must exit before restoring capacity configuration.' }
    Assert-PracticeHash $paths.backup $script:CapacityOriginalHash
    $current=(Get-FileHash -LiteralPath $paths.target).Hash
    if($current -notin @($script:CapacityOriginalHash,$script:CapacityModifiedHash)) { throw 'Changed capacity configuration retained; recovery requires inspection.' }
    if(Test-Path -LiteralPath $paths.temporary) {
        if((Get-FileHash -LiteralPath $paths.temporary).Hash -notin @($script:CapacityOriginalHash,$script:CapacityModifiedHash)) { throw 'Changed capacity temporary file retained.' }
        Remove-Item -LiteralPath $paths.temporary
    }
    if($current -eq $script:CapacityModifiedHash) {
        Write-PracticeFlowAtomic $paths $paths.backup $script:CapacityOriginalHash $script:CapacityModifiedHash
    }
    Assert-PracticeHash $paths.target $script:CapacityOriginalHash
}

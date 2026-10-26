# Guarded logo states 4a/4b and camera flythroughs dt/du become zero-delay
# waypoints. Native race setup, cleanup, countdown and attract movie stay.
$script:StartupOriginalHash='07e593b3891171a7c4caa67b0b2e0ee1a9c166fc2d4bd25814ad695c439d9adc'
$script:StartupLegacyModifiedHash='764651b98b6aa4ce8fe4ce6b5c9f48705d3a67b78a427628c3329c7ff9abdc0b'
$script:StartupModifiedHash='39d145561efaadce27df1e26a57ababdb383860b83cdf03b378daa48f2a819e0'
. (Join-Path $PSScriptRoot 'Game-Executable.ps1')

function Assert-StartupPlainPath([string]$Path) {
    $current=[IO.Path]::GetFullPath($Path)
    while($current) {
        if(Test-Path -LiteralPath $current) {
            if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked startup path retained: $current" }
        }
        $current=Split-Path $current -Parent
    }
}

function Get-StartupMoviesPaths([string]$Game,[string]$Run) {
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    $runPath=[IO.Path]::GetFullPath($Run).TrimEnd('\')
    if($runPath -eq $gamePath -or $runPath.StartsWith($gamePath+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Startup recovery backup must be outside the installation.' }
    if(!(Test-Path -LiteralPath $gamePath -PathType Container) -or !(Test-Path -LiteralPath $runPath -PathType Container)) { throw 'Startup game/run directory is missing.' }
    $paths=@{
        target=(Join-Path $gamePath 'system\states.bin')
        temporary=(Join-Path $gamePath 'system\GRID2VR.startup-movies.tmp')
        source=(Join-Path $runPath 'startup-movies.bin')
        backup=(Join-Path $runPath 'startup-movies-original.bin')
        executable=(Join-Path $gamePath 'grid2_avx.exe')
    }
    foreach($path in $paths.Values) { Assert-StartupPlainPath $path }
    return $paths
}

function Assert-StartupMoviesEntry($Entry,[switch]$AllowLegacy) {
    $supported=@($script:StartupModifiedHash)
    if($AllowLegacy) { $supported+=$script:StartupLegacyModifiedHash }
    if(!$Entry -or $Entry.name -cne 'system/states.bin' -or $Entry.sha256 -notin $supported -or $Entry.originalSha256 -ne $script:StartupOriginalHash) {
        throw 'Unsupported startup movie recovery entry.'
    }
}

function Assert-StartupHash([string]$Path,[string]$Expected) {
    Assert-StartupPlainPath $Path
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-FileHash -LiteralPath $Path).Hash -ne $Expected) { throw "Changed or missing startup file retained: $Path" }
}

function New-StartupMoviesDeployment([string]$Game,[string]$Run,[string]$Bundle='') {
    $paths=Get-StartupMoviesPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before preparing startup movie deployment.' }
    if((Test-Path -LiteralPath $paths.temporary) -or (Test-Path -LiteralPath $paths.backup)) { throw 'Existing startup recovery file retained.' }
    Assert-StartupHash $paths.target $script:StartupOriginalHash
    $null=Get-Grid2ExecutableHash $paths.executable
    if($Bundle) {
        $source=Join-Path ([IO.Path]::GetFullPath($Bundle)) 'states.bin'
        Assert-StartupHash $source $script:StartupModifiedHash
        if(Test-Path -LiteralPath $paths.source) { throw 'Existing startup run source retained.' }
        [IO.File]::Copy($source,$paths.source,$false)
    }
    Assert-StartupHash $paths.source $script:StartupModifiedHash
    [IO.File]::Copy($paths.target,$paths.backup,$false)
    Assert-StartupHash $paths.backup $script:StartupOriginalHash
    return @{name='system/states.bin';originalSha256=$script:StartupOriginalHash;sha256=$script:StartupModifiedHash}
}

function Write-StartupMoviesAtomic($Paths,[string]$Source,[string]$Expected,[string]$Before) {
    [IO.File]::Copy($Source,$Paths.temporary,$false)
    Assert-StartupHash $Paths.temporary $Expected
    $file=[IO.File]::Open($Paths.temporary,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $file.Flush($true) } finally { $file.Dispose() }
    Assert-StartupHash $Paths.target $Before
    [IO.File]::Replace($Paths.temporary,$Paths.target,[Management.Automation.Language.NullString]::Value)
    Assert-StartupHash $Paths.target $Expected
}

function Install-StartupMovies([string]$Game,[string]$Run,$Entry) {
    Assert-StartupMoviesEntry $Entry
    $paths=Get-StartupMoviesPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing GRID 2 process retained.' }
    $null=Get-Grid2ExecutableHash $paths.executable
    Assert-StartupHash $paths.target $script:StartupOriginalHash
    Assert-StartupHash $paths.backup $script:StartupOriginalHash
    Assert-StartupHash $paths.source $script:StartupModifiedHash
    if(Test-Path -LiteralPath $paths.temporary) { throw 'Existing startup temporary file retained.' }
    Write-StartupMoviesAtomic $paths $paths.source $script:StartupModifiedHash $script:StartupOriginalHash
}

function Restore-StartupMovies([string]$Game,[string]$Run,$Entry) {
    # Old logo-only runs remain recoverable after upgrading this helper.
    Assert-StartupMoviesEntry $Entry -AllowLegacy
    $modifiedHash=[string]$Entry.sha256
    $paths=Get-StartupMoviesPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'GRID 2 must exit before restoring startup movies.' }
    Assert-StartupHash $paths.backup $script:StartupOriginalHash
    if(!(Test-Path -LiteralPath $paths.target -PathType Leaf)) { throw 'Missing installed states; recovery requires inspection.' }
    $current=(Get-FileHash -LiteralPath $paths.target).Hash
    if($current -notin @($script:StartupOriginalHash,$modifiedHash)) { throw 'Changed installed states retained; recovery requires inspection.' }
    # Check all recovery inputs before removing even an owned partial transaction.
    if(Test-Path -LiteralPath $paths.temporary) {
        if((Get-FileHash -LiteralPath $paths.temporary).Hash -notin @($script:StartupOriginalHash,$modifiedHash)) { throw 'Changed startup temporary file retained.' }
        Remove-Item -LiteralPath $paths.temporary
    }
    if($current -eq $modifiedHash) {
        Write-StartupMoviesAtomic $paths $paths.backup $script:StartupOriginalHash $modifiedHash
    }
    Assert-StartupHash $paths.target $script:StartupOriginalHash
}

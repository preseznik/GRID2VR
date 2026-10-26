# Only fe/overlay_root/{top_safeframe/bar_top,bottom_safeframe/bar_base}.
# Their exclusive four-position payloads (!g and !M) total 96 bytes; 68 change.
# Replay controls retain their native enter/exit animation; only its OFF travel
# is extended below both native UI canvases after removing the opaque cover.
# Evidence: build/ui-cinema-vertices-evidence.json (parent retains durable receipt).
# Visibility evidence: build/replay-ui-hidden-state-evidence.json.
$script:CinemaOriginalHash='401c6244c8698963a9647fcdcf75541a5464155935916c3ced7cae8e47553980'
$script:CinemaModifiedHash='7fbd21a429036c1b7887195c085810ebacbd1878dcf4e7908f904983d759c0eb'
# Superseded diagnostic candidate: recovery only, never a new deployment.
$script:CinemaIndexOnlyHash='f74624a4fb810548c049369f4624925158dc4f485c2a0707c04feb7e75a58d6d'
$script:CinemaVerticesOnlyHash='5b79662d0dce087ec17c71abca1c72225cf50ff9ec9fc8f7d2e69b770074e14f'
. (Join-Path $PSScriptRoot 'Game-Executable.ps1')

function Assert-CinemaPlainPath([string]$Path) {
    $current=[IO.Path]::GetFullPath($Path)
    while($current) {
        if(Test-Path -LiteralPath $current) {
            if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked cinema matte path retained: $current" }
        }
        $current=Split-Path $current -Parent
    }
}

function Get-CinemaMattePaths([string]$Game,[string]$Run) {
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    $runPath=[IO.Path]::GetFullPath($Run).TrimEnd('\')
    if($runPath -eq $gamePath -or $runPath.StartsWith($gamePath+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Cinema matte recovery backup must be outside the installation.' }
    if(!(Test-Path -LiteralPath $gamePath -PathType Container) -or !(Test-Path -LiteralPath $runPath -PathType Container)) { throw 'Cinema matte game/run directory is missing.' }
    $paths=@{
        target=(Join-Path $gamePath 'frontend\databases\overlayPersistent.pssg')
        temporary=(Join-Path $gamePath 'frontend\databases\GRID2VR.cinema-matte.tmp')
        source=(Join-Path $runPath 'cinema-matte.pssg')
        backup=(Join-Path $runPath 'cinema-matte-original.pssg')
        executable=(Join-Path $gamePath 'grid2_avx.exe')
    }
    foreach($path in $paths.Values) { Assert-CinemaPlainPath $path }
    return $paths
}

function Assert-CinemaMatteEntry($Entry,[switch]$AllowLegacy) {
    $supported=@($script:CinemaModifiedHash)
    if($AllowLegacy) { $supported+= $script:CinemaIndexOnlyHash;$supported+= $script:CinemaVerticesOnlyHash }
    if(!$Entry -or $Entry.name -cne 'frontend/databases/overlayPersistent.pssg' -or $Entry.sha256 -notin $supported -or $Entry.originalSha256 -ne $script:CinemaOriginalHash) {
        throw 'Unsupported cinema matte recovery entry.'
    }
}

function Assert-CinemaHash([string]$Path,[string]$Expected) {
    Assert-CinemaPlainPath $Path
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-FileHash -LiteralPath $Path).Hash -ne $Expected) { throw "Changed or missing cinema matte file retained: $Path" }
}

function Assert-CinemaMatteOriginal([string]$Game) {
    Assert-CinemaHash (Join-Path $Game 'frontend\databases\overlayPersistent.pssg') $script:CinemaOriginalHash
    $null=Get-Grid2ExecutableHash (Join-Path $Game 'grid2_avx.exe')
}

function New-CinemaMatteDeployment([string]$Game,[string]$Run) {
    $paths=Get-CinemaMattePaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before preparing cinema matte deployment.' }
    foreach($path in @($paths.temporary,$paths.backup,$paths.source)) {
        if(Test-Path -LiteralPath $path) { throw "Existing cinema matte recovery file retained: $path" }
    }
    Assert-CinemaMatteOriginal $Game
    $bytes=[IO.File]::ReadAllBytes($paths.target)
    if($bytes.Length -ne 724829) { throw 'Unexpected cinema matte asset length.' }
    [byte[]]$indices=0,0,0,1,0,2,0,3,0,2,0,1
    foreach($offset in @(456040,457099)) {
        for($i=0;$i -lt $indices.Length;$i++) {
            if($bytes[$offset+$i] -ne $indices[$i]) { throw 'Cinema matte index guard mismatch.' }
        }
    }
    $positions=@{
        514752='c220000040c00000348ccde2c2200000c0be8cacb48ccde24220000040c00000348ccde242200000c0be8cacb48ccde2'
        516161='c220000040c00000348ccde2c2200000c0c00000b48ccde24220000040c00000348ccde242200000c0c00000b48ccde2'
    }
    foreach($offset in $positions.Keys) {
        if([BitConverter]::ToString($bytes,$offset,48).Replace('-','').ToLowerInvariant() -cne $positions[$offset]) { throw 'Cinema matte position guard mismatch.' }
    }
    $replayY=@{
        120095=@('3e0783c0bea9fcb8ba82b0003a2e4000','3e0783c0c28022ebbe45064b3e035987')
        120131=@('be4cccd0000000003ea9fcb8bea9fcb8','c280000000000000428022ebc28022eb')
    }
    foreach($offset in $replayY.Keys) {
        if([BitConverter]::ToString($bytes,$offset,16).Replace('-','').ToLowerInvariant() -cne $replayY[$offset][0]) { throw 'Replay overlay animation guard mismatch.' }
    }
    # Collapse all four vertices to one point, including if the frontend rebuilds
    # its quad indices. Keep original indices, counts, shaders and controls intact.
    foreach($offset in $positions.Keys) { [Array]::Clear($bytes,$offset,48) }
    # packet654 replay_anim[y]: affine Y-polynomial change around the identical
    # ON endpoint. OFF is -64; timings, X/Z tracks and enter/exit events stay native.
    foreach($offset in $replayY.Keys) {
        for($i=0;$i -lt 16;$i++) { $bytes[$offset+$i]=[Convert]::ToByte($replayY[$offset][1].Substring($i*2,2),16) }
    }
    $stream=[IO.File]::Open($paths.source,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $stream.Write($bytes,0,$bytes.Length);$stream.Flush($true) } finally { $stream.Dispose() }
    Assert-CinemaHash $paths.source $script:CinemaModifiedHash
    [IO.File]::Copy($paths.target,$paths.backup,$false)
    Assert-CinemaHash $paths.backup $script:CinemaOriginalHash
    return @{name='frontend/databases/overlayPersistent.pssg';originalSha256=$script:CinemaOriginalHash;sha256=$script:CinemaModifiedHash}
}

function Write-CinemaMatteAtomic($Paths,[string]$Source,[string]$Expected,[string]$Before) {
    [IO.File]::Copy($Source,$Paths.temporary,$false)
    Assert-CinemaHash $Paths.temporary $Expected
    $file=[IO.File]::Open($Paths.temporary,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $file.Flush($true) } finally { $file.Dispose() }
    Assert-CinemaHash $Paths.target $Before
    [IO.File]::Replace($Paths.temporary,$Paths.target,[Management.Automation.Language.NullString]::Value)
    Assert-CinemaHash $Paths.target $Expected
}

function Install-CinemaMatte([string]$Game,[string]$Run,$Entry) {
    Assert-CinemaMatteEntry $Entry
    $paths=Get-CinemaMattePaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing GRID 2 process retained.' }
    $null=Get-Grid2ExecutableHash $paths.executable
    Assert-CinemaHash $paths.target $script:CinemaOriginalHash
    Assert-CinemaHash $paths.backup $script:CinemaOriginalHash
    Assert-CinemaHash $paths.source $script:CinemaModifiedHash
    if(Test-Path -LiteralPath $paths.temporary) { throw 'Existing cinema matte temporary file retained.' }
    Write-CinemaMatteAtomic $paths $paths.source $script:CinemaModifiedHash $script:CinemaOriginalHash
}

function Restore-CinemaMatte([string]$Game,[string]$Run,$Entry) {
    Assert-CinemaMatteEntry $Entry -AllowLegacy
    $modifiedHash=[string]$Entry.sha256
    $paths=Get-CinemaMattePaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'GRID 2 must exit before restoring cinema mattes.' }
    Assert-CinemaHash $paths.backup $script:CinemaOriginalHash
    if(!(Test-Path -LiteralPath $paths.target -PathType Leaf)) { throw 'Missing installed UI asset; recovery requires inspection.' }
    $current=(Get-FileHash -LiteralPath $paths.target).Hash
    if($current -notin @($script:CinemaOriginalHash,$modifiedHash)) { throw 'Changed installed UI asset retained; recovery requires inspection.' }
    # Check all recovery inputs before removing even an owned partial transaction.
    if(Test-Path -LiteralPath $paths.temporary) {
        if((Get-FileHash -LiteralPath $paths.temporary).Hash -notin @($script:CinemaOriginalHash,$modifiedHash)) { throw 'Changed cinema matte temporary file retained.' }
        Remove-Item -LiteralPath $paths.temporary
    }
    if($current -eq $modifiedHash) {
        Write-CinemaMatteAtomic $paths $paths.backup $script:CinemaOriginalHash $modifiedHash
    }
    Assert-CinemaHash $paths.target $script:CinemaOriginalHash
}

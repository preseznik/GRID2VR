# Collapse only fe/overlay_root/{top_safeframe/bar_top,bottom_safeframe/bar_base}.
# Their exclusive four-position payloads (!g and !M) total 96 bytes; 68 change.
# Replay controls retain their native enter/exit animation; only its OFF travel
# is extended below both native UI canvases after removing the opaque cover.
# Evidence: build/ui-cinema-vertices-evidence.json (parent retains durable receipt).
# Visibility evidence: build/replay-ui-hidden-state-evidence.json.
# Popup default/out gets a separate hidden frame; original visible fraction
# curves remain unchanged. Evidence: docs/evidence/controls-hud-20261028/grey-strip-fix.json.
$script:CinemaOriginalHash='401c6244c8698963a9647fcdcf75541a5464155935916c3ced7cae8e47553980'
$script:CinemaModifiedHash='a74aaacfb7d80583955b77f0ab19eafa1ee6d381b0a70b6b95d75d45cf3aad6b'
$script:CinemaReplayOnlyHash='7fbd21a429036c1b7887195c085810ebacbd1878dcf4e7908f904983d759c0eb'
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
    if($AllowLegacy) { $supported+= $script:CinemaIndexOnlyHash;$supported+= $script:CinemaVerticesOnlyHash;$supported+= $script:CinemaReplayOnlyHash }
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
    # packet657 null[x/y/z]: keep every original polynomial unchanged. Prepend
    # a 1/1024-second hidden interval and translate original timestamps/visible
    # event targets equally. Fraction-out zero and default/exit zero stay hidden.
    # Offsets below refer to the original fingerprint, before the three inserts.
    $popupEdits=@{
        4=@('000b0f55','000b0fc1') # PSSG serialized length
        16477=@('000acefc','000acf68') # PSSGDATABASE serialized length
        16489=@('0001a188','0001a1f4') # LIBRARY serialized length
        121405=@('0000018f','000001fb') # NeAnimPacket_B1 serialized length
        121421=@('00000130','0000019c') # runtime packet allocation
        121462=@('00000156','000001c2') # NeAnimPacketData_B1 serialized length
        121504=@('00000002','00000003') # X key count
        121512=@('00000060','00000064') # X data starts after three timestamp floats
        121516=@('00000000','3a800000') # original x timestamp + epsilon
        121552=@('40555555','40556555') # original x timestamp + epsilon
        121614=@('00000002','00000003') # Y key count
        121618=@('000000a0','000000c4') # Y timestamp data moves by one serialized key
        121622=@('000000a8','000000d0') # Y data starts after three timestamp floats
        121626=@('00000000','3a800000') # original y timestamp + epsilon
        121662=@('40555555','40556555') # original y timestamp + epsilon
        121724=@('00000002','00000003') # Z key count
        121728=@('000000e8','00000130') # Z timestamp data moves by two serialized keys
        121732=@('000000f0','0000013c') # Z data starts after three timestamp floats
        121736=@('00000000','3a800000') # original z timestamp + epsilon
        121772=@('40555555','40556555') # original z timestamp + epsilon
        248467=@('00000000','3a800000') # feeptfas389.min
        248479=@('40555555','40556555') # feeptfas389.max
        257777=@('3e64b17e','3e65b17e') # feeptas.frame
        684626=@('bec96920','c2800000') # static hidden Y
    }
    foreach($offset in $popupEdits.Keys) {
        if([BitConverter]::ToString($bytes,$offset,4).Replace('-','').ToLowerInvariant() -cne $popupEdits[$offset][0]) { throw 'Popup hidden-frame guard mismatch.' }
    }
    # Collapse all four vertices to one point, including if the frontend rebuilds
    # its quad indices. Keep original indices, counts, shaders and controls intact.
    foreach($offset in $positions.Keys) { [Array]::Clear($bytes,$offset,48) }
    # packet654 replay_anim[y]: affine Y-polynomial change around the identical
    # ON endpoint. OFF is -64; timings, X/Z tracks and enter/exit events stay native.
    foreach($offset in $replayY.Keys) {
        for($i=0;$i -lt 16;$i++) { $bytes[$offset+$i]=[Convert]::ToByte($replayY[$offset][1].Substring($i*2,2),16) }
    }
    foreach($offset in $popupEdits.Keys) {
        for($i=0;$i -lt 4;$i++) { $bytes[$offset+$i]=[Convert]::ToByte($popupEdits[$offset][1].Substring($i*2,2),16) }
    }
    $popupKeys=@{
        121516='00000000bf000000c51c264ec4d035beb451d99a3f3fafd0000000000000000000000000' # x
        121626='00000000bf000000c51c264ec4d035beb451d99ac2800000427e6d2eb580000000000000' # y
        121736='00000000bf000000c51c264ec4d035beb451d99abe3b7e9d000000000000000000000000' # z
    }
    # Descending insertion keeps every original offset valid. The exact packet,
    # ancestor lengths and runtime timestamp/data offsets were guarded above.
    foreach($offset in @($popupKeys.Keys | Sort-Object -Descending)) {
        $expanded=[byte[]]::new($bytes.Length+36)
        [Array]::Copy($bytes,0,$expanded,0,$offset)
        for($i=0;$i -lt 36;$i++) { $expanded[$offset+$i]=[Convert]::ToByte($popupKeys[$offset].Substring($i*2,2),16) }
        [Array]::Copy($bytes,$offset,$expanded,$offset+36,$bytes.Length-$offset)
        $bytes=$expanded
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

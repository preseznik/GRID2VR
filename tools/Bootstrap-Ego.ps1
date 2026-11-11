# Fetches the pinned EGO Engine Modding source (MIT) used by the custom-track
# converter for PSSG, JPK collision and binary XML. Existing checkouts are
# verified, never replaced.
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$source=Join-Path $root '.deps\Ego-Engine-Modding'
$safe=$source.Replace('\','/')
$revision='f3fe9ee0f6e8379c64bc4a29e1d1b586bba5d1c1'
New-Item -ItemType Directory -Force -Path (Join-Path $root '.deps') | Out-Null
if(!(Test-Path -LiteralPath $source)) {
    & git -c http.sslBackend=openssl clone --no-checkout --filter=blob:none https://github.com/EgoEngineModding/Ego-Engine-Modding.git $source
    if($LASTEXITCODE) { throw 'EGO source clone failed.' }
    & git -c "safe.directory=$safe" -C $source -c http.sslBackend=openssl checkout --detach $revision
    if($LASTEXITCODE) { throw 'Pinned EGO checkout failed.' }
}
if((& git -c "safe.directory=$safe" -C $source rev-parse HEAD) -ne $revision) { throw 'Unexpected EGO revision; existing source preserved.' }
# Pinned fix: splitting collision into grid cells dropped the '*' (sheet) flag of
# surfaces in formats that store it per triangle (DiRT Showdown/GRID 2), so every
# '*' surface was written as '+'.
$patch=Join-Path $PSScriptRoot 'ego-sheet-materials.patch'
& git -c "safe.directory=$safe" -C $source apply --reverse --check $patch 2>$null
if($LASTEXITCODE) {
    if(& git -c "safe.directory=$safe" -C $source status --porcelain) { throw 'EGO source has other local changes; existing source preserved.' }
    & git -c "safe.directory=$safe" -C $source apply --check $patch
    if($LASTEXITCODE) { throw 'EGO source differs from the expected sheet-material patch; existing source preserved.' }
    & git -c "safe.directory=$safe" -C $source apply $patch
    if($LASTEXITCODE) { throw 'EGO sheet-material patch failed.' }
}
$changed=@(& git -c "safe.directory=$safe" -C $source diff --name-only)
if($changed.Count -ne 1 -or $changed[0] -cne 'src/EgoEngineLibrary/Formats/TrackQuadTree/QuadTreeMeshData.cs') { throw 'EGO source has unexpected local changes; existing source preserved.' }
Write-Output "Ego-Engine-Modding: verified $revision with sheet-material patch"

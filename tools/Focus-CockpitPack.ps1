# Local preview-pack installation. Never changes the game's PSSGs or saves.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install','Verify','Remove')][string]$Action,
    [Parameter(Mandatory)][string]$GameRoot,
    [string]$ZipPath=''
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Cockpit-Camera.ps1')
. (Join-Path $PSScriptRoot 'Paths.ps1')
$game=[IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
$mod=Join-Path $game 'GRID2VR'
$target=Join-Path $mod 'addons/focus-st-cockpit/0.1.0-poc.1'
$hashes=@{
    'foc_cabin.dds'='cf2a261e0174e09be8fdb63e3b87d106258b385254c640d73ac47dae9a69a976'
    'pack.json'='2f35070c18c9f366801592ce3ccce175e14d50c6ff6b9296bfb98dd031370e99'
    'README.txt'='2bb2b76e52b2c6f5ec09d7d85a6c2a1c55ff51b15776ad8e7d24367f32499c73'
}
function Assert-Pack([string]$Directory) {
    Assert-CockpitCameraPlainPath $Directory
    $entries=@(Get-ChildItem -LiteralPath $Directory -Force)
    if($entries.Count -ne 3) { throw 'Unrecognized pack contents retained.' }
    foreach($entry in $entries) {
        if($entry.PSIsContainer -or !$hashes.ContainsKey($entry.Name)) { throw 'Unrecognized pack entry retained.' }
        Assert-CockpitCameraHash $entry.FullName $hashes[$entry.Name]
    }
}
Assert-CockpitCameraPlainPath $target
Assert-CockpitCameraHash (Join-Path $game 'grid2_avx.exe') $script:CockpitCameraExecutableHash
# Require a core installation: a pack alone must not create an unowned GRID2VR
# folder that blocks the core installer on the next run.
if(!(Test-Path -LiteralPath (Join-Path $mod 'package.json') -PathType Leaf)) { throw 'Install the GRID2VR core before installing this optional pack.' }
if($Action -eq 'Verify') { Assert-Pack $target; Write-Output "Verified Focus ST pack: $target"; return }
$mutex=[Threading.Mutex]::new($false,'Global\GRID2VR.Session');$locked=$false
$staging=$null
try {
    try { $locked=$mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked=$true }
    if(!$locked) { throw 'Finish the GRID2VR session before changing its optional pack.' }
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before changing its optional pack.' }
    foreach($marker in @('GRID2VR.diagnostic-session','GRID2VR.session.xml')) {
        if(Test-Path -LiteralPath (Join-Path $game $marker)) { throw 'Restore the pending GRID2VR session first.' }
    }
    foreach($data in @((Get-Grid2DataRoot $mod),(Get-Grid2DataRoot (Split-Path $PSScriptRoot -Parent)))) {
        foreach($marker in @('session-pending.json','diagnostic-pending.json','practice-flow-pending.json','steamvr-theater-pending.json')) {
            if(Test-Path -LiteralPath (Join-Path $data $marker)) { throw 'Restore the pending GRID2VR session first.' }
        }
    }
    if($Action -eq 'Remove') {
        # Refuse changed/foreign content before deleting any of the three files.
        Assert-Pack $target
        foreach($name in $hashes.Keys) {
            $path=Join-Path $target $name;Assert-CockpitCameraHash $path $hashes[$name];[IO.File]::Delete($path)
        }
        [IO.Directory]::Delete($target,$false)
        foreach($parent in @((Split-Path $target -Parent),(Join-Path $mod 'addons'))) {
            if(!(Get-ChildItem -LiteralPath $parent -Force | Select-Object -First 1)) { [IO.Directory]::Delete($parent,$false) }
        }
        Write-Output 'Removed the verified Focus ST preview pack. Original textures will be used.'
        return
    }
    if(Test-Path -LiteralPath $target) { throw 'Existing pack retained; verify it or remove it explicitly first.' }
    if(!$ZipPath) { throw 'Install requires the local Focus ST pack ZIP.' }
    $zip=[IO.Path]::GetFullPath($ZipPath)
    Assert-CockpitCameraHash $zip '1fd7b2938e0701802baa82dd64905e89bd38ff62fd4b63b3d1fcd0c25f5ded70'
    if((Get-Item -LiteralPath $zip).Length -ne 1495769) { throw 'Unexpected ZIP size.' }
    $archive=[IO.Compression.ZipFile]::OpenRead($zip)
    try {
        if($archive.Entries.Count -ne 3) { throw 'Unexpected ZIP contents.' }
        $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach($entry in $archive.Entries) {
            if(!$hashes.ContainsKey($entry.Name) -or $entry.FullName -cne ('focus-st-cockpit/0.1.0-poc.1/'+$entry.Name) -or
                !$seen.Add($entry.Name) -or $entry.Length -gt 2796344) { throw 'Unexpected archive path or size.' }
        }
        # Stage beside the final version directory; the final move is atomic.
        $parent=Split-Path $target -Parent
        [IO.Directory]::CreateDirectory($parent) | Out-Null
        $staging=Join-Path $parent ('.install-'+[guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($staging) | Out-Null
        foreach($entry in $archive.Entries) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $staging $entry.Name),$false) }
        Assert-Pack $staging
        Assert-CockpitCameraPlainPath $target
        [IO.Directory]::Move($staging,$target);$staging=$null
    } finally { $archive.Dispose() }
    Assert-Pack $target
    Write-Output "Installed Focus ST preview pack: $target"
} finally {
    try { if($staging -and (Test-Path -LiteralPath $staging)) {
        # Only this invocation's fixed filenames; preserve unexpected files.
        Assert-CockpitCameraPlainPath $staging
        foreach($name in $hashes.Keys) { $path=Join-Path $staging $name;if(Test-Path -LiteralPath $path -PathType Leaf) { [IO.File]::Delete($path) } }
        if(!(Get-ChildItem -LiteralPath $staging -Force | Select-Object -First 1)) { [IO.Directory]::Delete($staging,$false) }
    } } finally { if($locked) { $mutex.ReleaseMutex() };$mutex.Dispose() }
}

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
$target=Join-Path $mod 'addons/cockpit-textures/0.2.0-preview.2'
$catalogHash='3a094699905d768ca9dd3286a6b0fad8b5cb4ed8ee33e5e14e2b9b2ca9df7536'
$catalogPath=Join-Path $PSScriptRoot 'data/cockpit-texture-pack.json'
Assert-CockpitCameraHash $catalogPath $catalogHash
$catalog=Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json -AsHashtable
if($catalog.schema -ne 1 -or $catalog.version -cne '0.2.0-preview.2' -or $catalog.files.Count -ne 75) { throw 'Unsupported cockpit texture catalog.' }
$hashes=$catalog.files
foreach($name in $hashes.Keys) {
    if($name -cnotmatch '^(?:[a-z0-9]{3}\.dds|pack\.json|README\.txt)$' -or $hashes[$name] -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid cockpit texture catalog.' }
}
function Assert-Pack([string]$Directory) {
    Assert-CockpitCameraPlainPath $Directory
    $entries=@(Get-ChildItem -LiteralPath $Directory -Force)
    if($entries.Count -ne $hashes.Count) { throw 'Unrecognized pack contents retained.' }
    foreach($entry in $entries) {
        if($entry.PSIsContainer -or !$hashes.ContainsKey($entry.Name)) { throw 'Unrecognized pack entry retained.' }
        Assert-CockpitCameraHash $entry.FullName $hashes[$entry.Name]
    }
}
Assert-CockpitCameraPlainPath $target
$null=Get-Grid2ExecutableHash (Join-Path $game 'grid2_avx.exe')
# Require a core installation: a pack alone must not create an unowned GRID2VR
# folder that blocks the core installer on the next run.
if(!(Test-Path -LiteralPath (Join-Path $mod 'package.json') -PathType Leaf)) { throw 'Install the GRID2VR core before installing this optional pack.' }
if($Action -eq 'Verify') { Assert-Pack $target; Write-Output "Verified cockpit texture pack: $target"; return }
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
        # Refuse changed/foreign content before deleting any of the owned files.
        Assert-Pack $target
        foreach($name in $hashes.Keys) {
            $path=Join-Path $target $name;Assert-CockpitCameraHash $path $hashes[$name];[IO.File]::Delete($path)
        }
        [IO.Directory]::Delete($target,$false)
        foreach($parent in @((Split-Path $target -Parent),(Join-Path $mod 'addons'))) {
            if(!(Get-ChildItem -LiteralPath $parent -Force | Select-Object -First 1)) { [IO.Directory]::Delete($parent,$false) }
        }
        Write-Output 'Removed the verified cockpit texture preview pack. Original textures will be used.'
        return
    }
    if(Test-Path -LiteralPath $target) { throw 'Existing pack retained; verify it or remove it explicitly first.' }
    if(!$ZipPath) { throw 'Install requires the local cockpit texture pack ZIP.' }
    $zip=[IO.Path]::GetFullPath($ZipPath)
    Assert-CockpitCameraHash $zip $catalog.zipSha256
    if((Get-Item -LiteralPath $zip).Length -ne $catalog.zipBytes) { throw 'Unexpected ZIP size.' }
    $archive=[IO.Compression.ZipFile]::OpenRead($zip)
    try {
        if($archive.Entries.Count -ne $hashes.Count) { throw 'Unexpected ZIP contents.' }
        $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach($entry in $archive.Entries) {
            if(!$hashes.ContainsKey($entry.Name) -or $entry.FullName -cne ('cockpit-textures/0.2.0-preview.2/'+$entry.Name) -or
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
    Write-Output "Installed cockpit texture preview pack: $target"
} finally {
    try { if($staging -and (Test-Path -LiteralPath $staging)) {
        # Only this invocation's fixed filenames; preserve unexpected files.
        Assert-CockpitCameraPlainPath $staging
        foreach($name in $hashes.Keys) { $path=Join-Path $staging $name;if(Test-Path -LiteralPath $path -PathType Leaf) { [IO.File]::Delete($path) } }
        if(!(Get-ChildItem -LiteralPath $staging -Force | Select-Object -First 1)) { [IO.Directory]::Delete($staging,$false) }
    } } finally { if($locked) { $mutex.ReleaseMutex() };$mutex.Dispose() }
}

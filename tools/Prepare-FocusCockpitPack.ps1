# Offline Focus ST texture proof of concept. Writes a new prepared folder only.
# Never deploys game files, changes launch options, saves, settings or starts the game.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameRoot,
    [Parameter(Mandatory)][string]$PackDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Library=(Join-Path (Split-Path $PSScriptRoot -Parent) '../DiRT2VR/artifacts/xml-convert/EgoEngineLibrary.dll')
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Cockpit-Camera.ps1')
$game=[IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
$pack=[IO.Path]::GetFullPath($PackDirectory).TrimEnd('\')
$output=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
$libraryPath=[IO.Path]::GetFullPath($Library)
foreach($p in @($game,$pack,$output,$libraryPath)){Assert-CockpitCameraPlainPath $p}
if($output -eq $game -or $output.StartsWith($game+'\',[StringComparison]::OrdinalIgnoreCase)) {throw 'Prepare outside the installed game; this proof of concept never installs.'}
if($output -eq $pack -or $output.StartsWith($pack+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Prepared game-derived files must remain outside the distributable pack.'}
if(Test-Path -LiteralPath $output){throw 'Output already exists; choose a new folder.'}
$relative='cars/models/foc/livery_user/textures_high/foc_tex_high_user.pssg'
$source=Join-Path $game $relative
$sourceHash='bbddfc40e7cfccb7f8d75ed545df1714abe7af949bae4c444d1279cb368298f7'
$libraryHash='b27a45447cd08a8e46423f16151a9d375a179a374ab83475b8029749ff528d98'
$null=Get-Grid2ExecutableHash (Join-Path $game 'grid2_avx.exe')
Assert-CockpitCameraHash $source $sourceHash
Assert-CockpitCameraHash $libraryPath $libraryHash
$manifestPath=Join-Path $pack 'pack.json'
Assert-CockpitCameraPlainPath $manifestPath
if((Get-Item -LiteralPath $manifestPath).Length -gt 16KB){throw 'Pack manifest too large.'}
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if($manifest.schema -ne 1 -or $manifest.id -cne 'focus-st-cockpit' -or $manifest.version -cne '0.1.0-poc.1' -or
   $manifest.texture -cne 'foc_cabin.dds' -or $manifest.originalSha256 -cne $sourceHash -or
   $manifest.originalPath -cne $relative -or $manifest.textureSha256 -cnotmatch '^[0-9a-f]{64}$') {throw 'Unsupported Focus proof-of-concept manifest.'}
$ddsPath=Join-Path $pack 'foc_cabin.dds'
Assert-CockpitCameraHash $ddsPath $manifest.textureSha256
$ddsBytes=[IO.File]::ReadAllBytes($ddsPath)
# Exact 2048-square BC1 layout: twelve mip levels, 128-byte legacy DDS header.
if($ddsBytes.Length -ne 2796344 -or [Text.Encoding]::ASCII.GetString($ddsBytes,0,4) -cne 'DDS ' -or
   [BitConverter]::ToUInt32($ddsBytes,4) -ne 124 -or [BitConverter]::ToUInt32($ddsBytes,12) -ne 2048 -or
   [BitConverter]::ToUInt32($ddsBytes,16) -ne 2048 -or [BitConverter]::ToUInt32($ddsBytes,28) -ne 12 -or
   [Text.Encoding]::ASCII.GetString($ddsBytes,84,4) -cne 'DXT1') {throw 'Expected a complete 2048 x 2048 BC1 DDS mip chain.'}
Add-Type -Path $libraryPath
function Read-Pssg($path){$s=[IO.File]::OpenRead($path);try{return [EgoEngineLibrary.Graphics.Pssg.PssgFile]::Open($s)}finally{$s.Dispose()}}
function Walk-Node($node){$node;foreach($child in $node.ChildElements){Walk-Node $child}}
function Get-NodeId($node){return ($node.Attributes|Where-Object Name -eq 'id').Value}
function NonCabin-Signature($file){
    $rows=[Collections.Generic.List[object]]::new()
    function Visit($n){
        if($n.Name -eq 'TEXTURE' -and (Get-NodeId $n) -ceq 'foc_cabin.tga'){return}
        $rows.Add([ordered]@{name=$n.Name;attrs=@(foreach($a in $n.Attributes){[ordered]@{name=$a.Name;value=$(if($a.Value -is [byte[]]){[Convert]::ToBase64String($a.Value)}else{$a.Value})}});data=[Convert]::ToBase64String($n.Value);children=$n.ChildElements.Count})
        foreach($c in $n.ChildElements){Visit $c}
    }
    Visit $file.RootElement
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($rows.ToArray()|ConvertTo-Json -Depth 12 -Compress)))).ToLowerInvariant()
}
$file=Read-Pssg $source
$before=NonCabin-Signature $file
$textures=@(Walk-Node $file.RootElement|Where-Object {$_.Name -eq 'TEXTURE' -and (Get-NodeId $_) -ceq 'foc_cabin.tga'})
if($textures.Count -ne 1){throw 'Expected one Focus cabin texture.'}
$oldAttrs=[ordered]@{};foreach($a in $textures[0].Attributes){if($a.Name -notin @('width','height','numberMipMapLevels')){$oldAttrs[$a.Name]=$a.Value}}
$stream=[IO.File]::OpenRead($ddsPath)
try{$dds=[EgoEngineLibrary.Graphics.Dds.DdsFile]::new($stream)}finally{$stream.Dispose()}
[EgoEngineLibrary.Graphics.GraphicsExtensions]::ToPssgElement($dds,$textures[0])
if((NonCabin-Signature $file) -ne $before){throw 'Unrelated PSSG content changed before save.'}
[IO.Directory]::CreateDirectory($output)|Out-Null
$target=Join-Path $output 'foc_tex_high_user.pssg'
$stream=[IO.File]::Open($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try{$file.Save($stream)}finally{$stream.Dispose()}
$reopened=Read-Pssg $target
if((NonCabin-Signature $reopened) -ne $before){throw 'Unrelated PSSG content changed after reparse.'}
$texture=@(Walk-Node $reopened.RootElement|Where-Object {$_.Name -eq 'TEXTURE' -and (Get-NodeId $_) -ceq 'foc_cabin.tga'})[0]
$newAttrs=[ordered]@{};foreach($a in $texture.Attributes){if($a.Name -notin @('width','height','numberMipMapLevels')){$newAttrs[$a.Name]=$a.Value}}
if(($oldAttrs|ConvertTo-Json -Compress) -cne ($newAttrs|ConvertTo-Json -Compress)){throw 'Cabin sampler/format attributes changed.'}
$memory=[IO.MemoryStream]::new()
try{[EgoEngineLibrary.Graphics.GraphicsExtensions]::ToDdsFile($texture).Write($memory);$verified=$memory.ToArray()}finally{$memory.Dispose()}
if($verified.Length -ne $ddsBytes.Length -or -not [Linq.Enumerable]::SequenceEqual([byte[]]$verified[128..($verified.Length-1)],[byte[]]$ddsBytes[128..($ddsBytes.Length-1)])){throw 'Repacked cabin mip data differs.'}
Assert-CockpitCameraHash $source $sourceHash
Assert-CockpitCameraHash $ddsPath $manifest.textureSha256
$receipt=[ordered]@{
    schema=1;packId=$manifest.id;version=$manifest.version;originalPath=$relative;originalSha256=$sourceHash
    textureSha256=$manifest.textureSha256;preparedSha256=(Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant()
    preparedBytes=(Get-Item -LiteralPath $target).Length;nonCabinSemanticSha256=$before
    onlyCabinChanged=$true;samplerAttributesPreserved=$true;ddsPayloadVerified=$true;sourceUnchanged=$true
    width=2048;height=2048;ddsMipLevels=12;format='BC1/DXT1';gameLaunched=$false;deployed=$false
    warning='Local game-derived PSSG; do not include in the downloadable pack. Runtime loading/save compatibility and visual acceptance remain unverified.'
}
$receipt|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'prepared.json') -Encoding utf8
$receipt|ConvertTo-Json -Depth 5

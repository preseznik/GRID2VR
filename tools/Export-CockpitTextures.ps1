# Development-only read-only export. Never deploys or rewrites a game asset.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$AdditionalSources='',
    [string]$Library='F:/Coding/Codex/Mods/DiRT2VR/artifacts/xml-convert/EgoEngineLibrary.dll'
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Cockpit-Camera.ps1')
$game=[IO.Path]::GetFullPath($GameRoot).TrimEnd('\')
$output=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
if($output -eq $game -or $output.StartsWith($game+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Export outside the installed game.'}
Assert-CockpitCameraPlainPath $output
Assert-CockpitCameraHash $Library 'b27a45447cd08a8e46423f16151a9d375a179a374ab83475b8029749ff528d98'
if(Test-Path -LiteralPath $output){throw 'Choose a new output directory.'}
Add-Type -Path $Library
$sources=@(Get-ChildItem -LiteralPath (Join-Path $game 'cars/models') -Directory | Sort-Object Name | ForEach-Object {
    $car=$_.Name
    [pscustomobject]@{car=$car;path=(Join-Path $_.FullName "livery_user/textures_high/${car}_tex_high_user.pssg");origin="cars/models/$car/livery_user/textures_high/${car}_tex_high_user.pssg";dlc=$false}
})
if($AdditionalSources){$sources+=@(Get-Content -LiteralPath $AdditionalSources -Raw | ConvertFrom-Json)}
[IO.Directory]::CreateDirectory($output)|Out-Null
$rows=[Collections.Generic.List[object]]::new()
function Walk-Texture($node){if($node.Name -eq 'TEXTURE'){$node};foreach($c in $node.ChildElements){Walk-Texture $c}}
foreach($entry in $sources){
    if($entry.car -cnotmatch '^[a-z0-9]{3}$'){throw 'Invalid car identifier'}
    Assert-CockpitCameraPlainPath $entry.path
    $before=(Get-FileHash -LiteralPath $entry.path).Hash.ToLowerInvariant()
    $stream=[IO.File]::OpenRead($entry.path)
    try{$pssg=[EgoEngineLibrary.Graphics.Pssg.PssgFile]::Open($stream)}finally{$stream.Dispose()}
    $textures=@(Walk-Texture $pssg.RootElement | Where-Object {($_.Attributes|Where-Object Name -eq 'id').Value -match 'cabin'})
    if($textures.Count -ne 1){throw "Expected one cabin atlas for $($entry.car), found $($textures.Count)"}
    $texture=$textures[0]
    $target=Join-Path $output "$($entry.car).dds"
    $stream=[IO.File]::Open($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
    try{[EgoEngineLibrary.Graphics.GraphicsExtensions]::ToDdsFile($texture).Write($stream)}finally{$stream.Dispose()}
    if((Get-FileHash -LiteralPath $entry.path).Hash.ToLowerInvariant() -ne $before){throw 'Game source changed during export'}
    $attrs=[ordered]@{};foreach($a in $texture.Attributes){$attrs[$a.Name]=$a.Value}
    $rows.Add([ordered]@{car=$entry.car;origin=$entry.origin;dlc=[bool]$entry.dlc;sourceSha256=$before;texture=$attrs;ddsSha256=(Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant()})
}
[ordered]@{schema=1;readOnly=$true;cars=$rows.ToArray()} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'inventory.json') -Encoding utf8
Write-Output "Exported $($rows.Count) cabin textures read-only: $output"

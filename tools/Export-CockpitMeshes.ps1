# Offline mesh export only. Never deploys, launches, or changes game assets.
[CmdletBinding()]
param(
 [string]$GameRoot='E:/SteamLibrary/steamapps/common/grid 2',
 [string]$Library='F:/Coding/Codex/Mods/DiRT2VR/artifacts/xml-convert/EgoEngineLibrary.dll',
 [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Cockpit-Camera.ps1')
$game=[IO.Path]::GetFullPath($GameRoot)
$out=[IO.Path]::GetFullPath($OutputDirectory)
foreach($path in @($game,$out,$Library)){Assert-CockpitCameraPlainPath $path}
if($out.TrimEnd('\') -eq $game.TrimEnd('\') -or $out.StartsWith($game.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {throw 'Mesh evidence must be outside the installed game.'}
if(Test-Path -LiteralPath $out){throw 'Choose a new mesh evidence folder.'}
Assert-CockpitCameraHash $Library 'b27a45447cd08a8e46423f16151a9d375a179a374ab83475b8029749ff528d98'
$catalog=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'data/cockpit-defaults.json') -Raw | ConvertFrom-Json
if($catalog.schema -ne 1 -or @($catalog.cars).Count -ne 51){throw 'Unexpected mesh catalog.'}
[IO.Directory]::CreateDirectory($out)|Out-Null
Add-Type -Path $Library
function Export-Node($node) {
 $attrs=[ordered]@{}
 foreach($a in $node.Attributes) {
   $v=$a.Value
   if($v -is [byte[]]) {$v=@{raw=[Convert]::ToBase64String($v)}}
   $attrs[$a.Name]=$v
 }
 $r=[ordered]@{type=$node.Name;attrs=$attrs}
 if($node.Value.Length -gt 0) {$r.data=[Convert]::ToBase64String($node.Value); if($node.Value.Length -le 80){$r.display=$node.DisplayValue.Trim()}}
 if($node.ChildElements.Count -gt 0) {$r.children=@(foreach($child in $node.ChildElements){Export-Node $child})}
 return $r
}
$total=0L
Push-Location -LiteralPath $out
try {
 foreach($car in $catalog.cars) {
  if($car.car -cnotmatch '^[a-z0-9]{3}$'){throw 'Invalid car'}
  $rel="cars/models/$($car.car)/$($car.car)_highLOD.pssg"
  $p=Join-Path $game $rel
  Assert-CockpitCameraHash $p $car.meshSha256
  if((Get-Item -LiteralPath $p).Length -gt 8MB){throw 'Oversized mesh input.'}
  $stream=[IO.File]::OpenRead($p)
  try {$file=[EgoEngineLibrary.Graphics.Pssg.PssgFile]::Open($stream);$result=@{source=$rel;sha256=$car.meshSha256;root=(Export-Node $file.RootElement)}}
  finally {$stream.Dispose()}
  Assert-CockpitCameraHash $p $car.meshSha256
  $target=Join-Path $out "$($car.car)_highLOD.json"
  $result|ConvertTo-Json -Depth 100 -Compress|Set-Content -LiteralPath $target
  $total+=(Get-Item -LiteralPath $target).Length
  if($total -gt 256MB){throw 'Mesh export exceeded diagnostic storage budget.'}
 }
} finally {Pop-Location}
Write-Output "Exported 51 car meshes ($total bytes); game unchanged."

[CmdletBinding()]
param(
    [string]$GameRoot = 'E:\SteamLibrary\steamapps\common\grid 2',
    [string]$Library = 'F:\Coding\Codex\Mods\DiRT2VR\artifacts\xml-convert\EgoEngineLibrary.dll',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\cockpit-catalog-20261025')
)
$ErrorActionPreference = 'Stop'
function Assert-GeometryPlainPath([string]$Path) {
    for ($part=[IO.Path]::GetFullPath($Path); $part; $part=[IO.Path]::GetDirectoryName($part)) {
        if ((Test-Path -LiteralPath $part) -and
            ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Linked geometry input/output: $part"
        }
    }
}
$game = [IO.Path]::GetFullPath($GameRoot)
$out = [IO.Path]::GetFullPath($OutputDirectory)
foreach ($path in @($game, $out, $Library)) { Assert-GeometryPlainPath $path }
if ($out.TrimEnd('\') -eq $game.TrimEnd('\') -or
    $out.StartsWith($game.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Geometry evidence must be outside the installed game.'
}
if (Test-Path -LiteralPath (Join-Path $out 'geometry-input.json')) { throw 'Geometry evidence already exists.' }
if ((Get-FileHash -LiteralPath $Library -Algorithm SHA256).Hash.ToLowerInvariant() -ne
    'b27a45447cd08a8e46423f16151a9d375a179a374ab83475b8029749ff528d98') {
    throw 'Unrecognized PSSG parser fingerprint.'
}
New-Item -ItemType Directory -Path $out -Force | Out-Null
Add-Type -Path $Library

function Get-Id($Node) {
    $a = $Node.Attributes | Where-Object Name -eq 'id'
    if ($a) { return [string]$a.Value }
    return ''
}
function Get-Source($Relative) {
    $path = Join-Path $game $Relative
    Assert-GeometryPlainPath $path
    return [ordered]@{path=$Relative; bytes=(Get-Item -LiteralPath $path).Length; sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function Read-Pssg($Relative, [scriptblock]$Read) {
    $source = Get-Source $Relative
    $stream = [IO.File]::OpenRead((Join-Path $game $Relative))
    try { $file = [EgoEngineLibrary.Graphics.Pssg.PssgFile]::Open($stream); $data = & $Read $file.RootElement }
    finally { $stream.Dispose() }
    if ((Get-Source $Relative).sha256 -ne $source.sha256) { throw "Asset changed while reading: $Relative" }
    return [ordered]@{source=$source; data=$data}
}
function Get-TransformNodes($Root) {
    $records = [Collections.Generic.List[object]]::new()
    function Visit-Transform($Node, [string]$ParentPath) {
        $id = Get-Id $Node
        $path = $ParentPath + '/' + $Node.Name + ':' + $id
        $transforms = @($Node.ChildElements | Where-Object Name -eq 'TRANSFORM')
        if ($transforms.Count) {
            if ($transforms.Count -ne 1) { throw "Multiple transforms: $path" }
            $records.Add([ordered]@{id=$id; path=$path; parent=$ParentPath; matrix=$transforms[0].DisplayValue.Trim()})
        }
        foreach ($child in $Node.ChildElements) { Visit-Transform $child $path }
    }
    Visit-Transform $Root ''
    return @($records.ToArray())
}
function Get-NeutralPackets($Root) {
    $nodes = [Collections.Generic.List[object]]::new()
    function Visit-Neutral($Node) {
        $nodes.Add($Node)
        foreach ($child in $Node.ChildElements) { Visit-Neutral $child }
    }
    Visit-Neutral $Root
    $clips = @($nodes | Where-Object { $_.Name -eq 'NeAnimClip' -and (Get-Id $_) -eq 'd_Neutral' })
    if ($clips.Count -ne 1) { throw 'Expected one authored d_Neutral clip.' }
    $packets = @()
    foreach ($reference in $clips[0].ChildElements) {
        $raw = ($reference.Attributes | Where-Object Name -eq 'packet').Value
        if ($raw -isnot [byte[]] -or $raw.Length -lt 5) { throw 'Unexpected animation packet reference.' }
        $count = ([uint32]$raw[0] -shl 24) -bor ([uint32]$raw[1] -shl 16) -bor ([uint32]$raw[2] -shl 8) -bor [uint32]$raw[3]
        if ($count -ne $raw.Length - 4 -or $raw[4] -ne 35) { throw 'Malformed animation packet reference.' }
        $id = [Text.Encoding]::ASCII.GetString($raw, 5, $raw.Length-5)
        $matches = @($nodes | Where-Object { (Get-Id $_) -eq $id })
        if ($matches.Count -ne 1 -or $matches[0].ChildElements.Count -ne 1) { throw "Unexpected packet: $id" }
        $node = $matches[0]
        $packets += [ordered]@{id=$id; type=$node.Name; data=[Convert]::ToBase64String($node.ChildElements[0].Value)}
    }
    return [ordered]@{clip='d_Neutral'; packets=$packets}
}
$skeleton = Read-Pssg 'characters/models/driver_model.pssg' { param($root) Get-TransformNodes $root }
$cars = @()
foreach ($directory in (Get-ChildItem -LiteralPath (Join-Path $game 'cars\models') -Directory | Sort-Object Name)) {
    $car = $directory.Name
    if ($car -notmatch '^[a-z0-9]{3}$') { throw "Unexpected car code: $car" }
    $record = [ordered]@{car=$car}
    try {
        $record.animation = Read-Pssg "cars/interiors/models/$car/int_${car}_driver_anims.pssg" { param($root) Get-NeutralPackets $root }
        $record.dummies = Read-Pssg "cars/models/$car/${car}_anim_dummies.pssg" { param($root) Get-TransformNodes $root }
        $record.camera = Get-Source "cars/models/$car/cameras.xml"
        $record.status = 'exported'
    } catch { $record.status = 'unsupported'; $record.error = $_.Exception.Message }
    $cars += $record
}
$result = [ordered]@{
    schema=1; gameRoot=$game; exporterSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash.ToLowerInvariant()
    library=[ordered]@{path=[IO.Path]::GetFullPath($Library);sha256=(Get-FileHash -LiteralPath $Library).Hash.ToLowerInvariant()}
    skeleton=$skeleton; cars=$cars
}
$result | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $out 'geometry-input.json') -Encoding utf8
[pscustomobject]@{output=$out; cars=$cars.Count; exported=@($cars | Where-Object status -eq exported).Count; failures=@($cars | Where-Object status -eq unsupported)} | ConvertTo-Json -Depth 4

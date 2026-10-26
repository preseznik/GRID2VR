# Installed packages and the development workspace share the same session code.
function Test-Grid2Package([string]$Root) { return Test-Path -LiteralPath (Join-Path $Root 'package.json') -PathType Leaf }
function Get-Grid2GameRoot([string]$Root) {
    if(Test-Grid2Package $Root) { return Split-Path ([IO.Path]::GetFullPath($Root).TrimEnd('\')) -Parent }
    return 'E:\SteamLibrary\steamapps\common\grid 2'
}
function Get-Grid2DataRoot([string]$Root) {
    if(!(Test-Grid2Package $Root)) { return Join-Path $Root 'artifacts' }
    $game=(Get-Grid2GameRoot $Root).ToUpperInvariant()
    $id=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($game))).Substring(0,24)
    $base=if($env:GRID2VR_USER_BASE){$env:GRID2VR_USER_BASE}else{Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'GRID2VR'}
    if(![IO.Path]::IsPathFullyQualified($base)) { throw 'User-data location must be absolute.' }
    return Join-Path $base $id
}
function Get-Grid2LogDirectory([string]$Root,[string]$Path) {
    if(![IO.Path]::IsPathFullyQualified($Path)) { throw 'Log folder must be absolute.' }
    $full=[IO.Path]::GetFullPath($Path).TrimEnd('\')
    $logs=[IO.Path]::GetFullPath((Join-Path (Get-Grid2DataRoot $Root) 'logs'))
    if([IO.Path]::GetDirectoryName($full) -ne $logs -or [IO.Path]::GetFileName($full) -cnotmatch '^\d{8}-\d{6}-\d{3}$') { throw 'Log folder is outside its owned timestamp directory.' }
    for($part=$full; $part; $part=[IO.Path]::GetDirectoryName($part)) {
        if((Test-Path -LiteralPath $part) -and ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Linked log folder retained: $part" }
    }
    return $full
}
function New-Grid2LogDirectory([string]$Root) {
    $stamp=Get-Date
    for($attempt=0; $attempt -lt 100; $attempt++) {
        $path=Get-Grid2LogDirectory $Root (Join-Path (Get-Grid2DataRoot $Root) ('logs/'+$stamp.AddMilliseconds($attempt).ToString('yyyyMMdd-HHmmss-fff')))
        if(Test-Path -LiteralPath $path) { continue }
        New-Item -ItemType Directory -Path $path -ErrorAction Stop | Out-Null
        return $path
    }
    throw 'Could not allocate a new session log folder.'
}
function Get-Grid2Python([string]$Root) {
    if(Test-Grid2Package $Root) { return Join-Path $Root 'payload/python/python.exe' }
    $line=Select-String -LiteralPath (Join-Path $Root 'build/win32/CMakeCache.txt') -Pattern '^_?Python3_EXECUTABLE:[^=]*=(.+)$' | Select-Object -First 1
    if(!$line) { throw 'Build configuration does not identify Python.' }
    return $line.Matches[0].Groups[1].Value.Trim()
}
function Get-Grid2Proxy([string]$Root) {
    if(Test-Grid2Package $Root) { return Get-Grid2PackageFile $Root 'payload/d3d11.dll' }
    return Join-Path $Root 'build/win32/bin/RelWithDebInfo/d3d11.dll'
}
function Get-Grid2Launcher([string]$Root) {
    if(Test-Grid2Package $Root) { return Join-Path (Get-Grid2GameRoot $Root) 'GRID2VR.exe' }
    return Join-Path $Root 'build/launcher/GRID2VR.exe'
}
function Get-Grid2Converter([string]$Root) {
    if(Test-Grid2Package $Root) { return Get-Grid2PackageFile $Root 'payload/xml-convert/xml-convert.exe' }
    return Join-Path (Split-Path $Root -Parent) 'DiRT2VR/artifacts/xml-convert/xml-convert.exe'
}
function Get-Grid2PackageFile([string]$Root,[string]$Relative) {
    $manifest=Get-Content -LiteralPath (Join-Path $Root 'package.json') -Raw | ConvertFrom-Json -AsHashtable
    $key='GRID2VR/'+$Relative.Replace('\','/')
    $path=[IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if(!$path.StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
       $manifest.schema -ne 1 -or $manifest.files[$key] -notmatch '^[0-9a-f]{64}$' -or
       (Get-FileHash -LiteralPath $path -ErrorAction Stop).Hash -ne $manifest.files[$key]) { throw 'Installed package file is missing or changed.' }
    for($part=$path;$part;$part=[IO.Path]::GetDirectoryName($part)) {
        if((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked package file retained.' }
    }
    return $path
}
function Get-Grid2SteamRoot {
    $path=(Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if(!$path) { $path=Join-Path ${env:ProgramFiles(x86)} 'Steam' }
    return [IO.Path]::GetFullPath($path)
}
$distributionRoot=Split-Path $PSScriptRoot -Parent
if(Test-Grid2Package $distributionRoot) {
    $env:GRID2VR_DATA_ROOT=Get-Grid2DataRoot $distributionRoot
    $env:DOTNET_ROOT=Join-Path $distributionRoot 'payload/dotnet'
    $env:DOTNET_ROOT_X64=$env:DOTNET_ROOT
    $env:DOTNET_MULTILEVEL_LOOKUP='0'
    $env:PYTHONNOUSERSITE='1'
    $env:PYTHONDONTWRITEBYTECODE='1'
    $env:PYTHONHOME=$null
    $env:PYTHONPATH=$null
}

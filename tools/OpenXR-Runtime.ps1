. (Join-Path $PSScriptRoot 'Paths.ps1')
# Resolve only the two supported x86 runtimes. Reading this file or validating
# a manifest never loads OpenXR or changes the system runtime selection.
function Get-Grid2VrRuntimeManifest {
    param([ValidateSet('VDXR','SteamVR')][string]$Runtime='VDXR')
    if($Runtime -eq 'VDXR') { return Join-Path $env:ProgramFiles 'Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.json' }
    $steam=Get-Grid2SteamRoot
    $libraries=@($steam)
    $vdf=Join-Path $steam 'steamapps/libraryfolders.vdf'
    if(Test-Path -LiteralPath $vdf) {
        foreach($match in [regex]::Matches([IO.File]::ReadAllText($vdf),'"path"\s*"([^"]+)"')) { $libraries+=$match.Groups[1].Value.Replace('\\','\') }
    }
    foreach($library in $libraries) { $path=Join-Path $library 'steamapps/common/SteamVR/steamxr_win32.json';if(Test-Path -LiteralPath $path){return $path} }
    return Join-Path $steam 'steamapps/common/SteamVR/steamxr_win32.json'
}
function Resolve-Grid2VrRuntime {
    param([Parameter(Mandatory)][string]$ManifestPath)
    if(![IO.Path]::IsPathRooted($ManifestPath)) { throw 'An absolute OpenXR manifest path is required.' }
    $path=[IO.Path]::GetFullPath($ManifestPath)
    $kind=switch([IO.Path]::GetFileName($path)) {
        'virtualdesktop-openxr-32.json' { 'VDXR' }
        'steamxr_win32.json' { 'SteamVR' }
        default { throw 'Only the VDXR and SteamVR Win32 manifests are supported.' }
    }
    $file=Get-Item -LiteralPath $path -ErrorAction Stop
    if($file.PSIsContainer -or $file.Length -gt 64KB -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Invalid runtime manifest file.' }
    $manifest=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if($manifest.file_format_version -ne '1.0.0' -or !$manifest.runtime.library_path) { throw 'Invalid OpenXR runtime manifest.' }
    $library=$manifest.runtime.library_path
    if(![IO.Path]::IsPathRooted($library)) { $library=Join-Path $file.DirectoryName $library }
    $library=[IO.Path]::GetFullPath($library)
    $dll=Get-Item -LiteralPath $library -ErrorAction Stop
    if($dll.PSIsContainer -or ($dll.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Invalid runtime library file.' }
    $reader=[IO.BinaryReader]::new([IO.File]::OpenRead($library))
    try {
        if($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw 'Runtime library is not a PE image.' }
        $reader.BaseStream.Position=0x3c
        $pe=$reader.ReadUInt32()
        if($pe -lt 64 -or $pe -gt $reader.BaseStream.Length-26) { throw 'Invalid runtime PE header.' }
        $reader.BaseStream.Position=$pe
        if($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) { throw 'GRID 2 requires an x86 runtime library.' }
        $reader.BaseStream.Position=$pe+24
        if($reader.ReadUInt16() -ne 0x10b) { throw 'GRID 2 requires a PE32 runtime library.' }
    } finally { $reader.Dispose() }
    [pscustomobject]@{name=$kind;manifest=$path;library=$library;architecture='x86';theaterSuppression=($kind -eq 'SteamVR')}
}

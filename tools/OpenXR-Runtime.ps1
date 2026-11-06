. (Join-Path $PSScriptRoot 'Paths.ps1')
# Runtime discovery and validation never load OpenXR or change its system default.
function Get-Grid2VrSystemRuntimeManifest {
    # PowerShell/launcher are x64; GRID 2 and its OpenXR loader are x86.
    $registry=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry32)
    try {
        $key=$registry.OpenSubKey('SOFTWARE\Khronos\OpenXR\1')
        try { if($key) { return $key.GetValue('ActiveRuntime') } } finally { if($key) { $key.Dispose() } }
    } finally { $registry.Dispose() }
}
function Get-Grid2VrDefaultRuntimeManifest {
    # Honor the loader's process-local override before the 32-bit system setting.
    $path=$env:XR_RUNTIME_JSON
    if([string]::IsNullOrWhiteSpace($path)) { $path=Get-Grid2VrSystemRuntimeManifest }
    if([string]::IsNullOrWhiteSpace($path)) {
        throw 'No default 32-bit OpenXR runtime is configured. Make your headset software the active OpenXR runtime, or choose VDXR or SteamVR.'
    }
    return [Environment]::ExpandEnvironmentVariables($path.Trim())
}
function Get-Grid2VrRuntimeManifest {
    param([ValidateSet('VDXR','SteamVR','Default')][string]$Runtime='VDXR')
    if($Runtime -eq 'Default') { return Get-Grid2VrDefaultRuntimeManifest }
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
    param([Parameter(Mandatory)][string]$ManifestPath,[switch]$DefaultRuntime)
    if(![IO.Path]::IsPathFullyQualified($ManifestPath)) { throw 'An absolute OpenXR manifest path is required.' }
    $path=[IO.Path]::GetFullPath($ManifestPath)
    $kind=switch([IO.Path]::GetFileName($path)) {
        'virtualdesktop-openxr-32.json' { 'VDXR' }
        'steamxr_win32.json' { 'SteamVR' }
        default {
            if(!$DefaultRuntime -or [IO.Path]::GetExtension($path) -ine '.json') { throw 'Choose VDXR, SteamVR or Default OpenXR with a .json runtime manifest.' }
            'Default'
        }
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
        if($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14c) { throw 'GRID 2 requires a 32-bit (x86) OpenXR runtime library.' }
        $reader.BaseStream.Position=$pe+24
        if($reader.ReadUInt16() -ne 0x10b) { throw 'GRID 2 requires a 32-bit (PE32) OpenXR runtime library.' }
    } finally { $reader.Dispose() }
    [pscustomobject]@{name=$kind;selection=$(if($DefaultRuntime){'Default'}else{$kind});manifest=$path;library=$library;architecture='x86';theaterSuppression=($kind -eq 'SteamVR')}
}

$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\OpenXR-Runtime.ps1')
$fixture=Join-Path $root ('artifacts\runtime-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$originalOverride=$env:XR_RUNTIME_JSON
$systemReader=(Get-Item Function:Get-Grid2VrSystemRuntimeManifest).ScriptBlock
try {
    $script:systemManifest='';$script:systemReads=0
    function Get-Grid2VrSystemRuntimeManifest { $script:systemReads++;return $script:systemManifest }
    $dll=Join-Path $fixture 'runtime.dll'
    $bytes=[byte[]]::new(160)
    [BitConverter]::GetBytes([uint16]0x5a4d).CopyTo($bytes,0)
    [BitConverter]::GetBytes([uint32]64).CopyTo($bytes,0x3c)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes,64)
    [BitConverter]::GetBytes([uint16]0x14c).CopyTo($bytes,68)
    [BitConverter]::GetBytes([uint16]0x10b).CopyTo($bytes,88)
    [IO.File]::WriteAllBytes($dll,$bytes)
    foreach($kind in @('VDXR','SteamVR')) {
        $path=Join-Path $fixture ([IO.Path]::GetFileName((Get-Grid2VrRuntimeManifest $kind)))
        @{file_format_version='1.0.0';runtime=@{library_path='.\runtime.dll'}} | ConvertTo-Json | Set-Content -LiteralPath $path
        $resolved=Resolve-Grid2VrRuntime $path
        if($resolved.name -ne $kind -or $resolved.library -ne $dll -or $resolved.architecture -ne 'x86' -or $resolved.theaterSuppression -ne ($kind -eq 'SteamVR')) { throw 'Runtime selection mismatch.' }
    }
    $vdxr=Join-Path $fixture 'virtualdesktop-openxr-32.json'
    function Assert-Rejected([string]$path,[switch]$DefaultRuntime) {
        $rejected=$false
        try { Resolve-Grid2VrRuntime $path -DefaultRuntime:$DefaultRuntime | Out-Null } catch { $rejected=$true }
        if(!$rejected) { throw "Unsafe runtime accepted: $path" }
    }
    Assert-Rejected 'virtualdesktop-openxr-32.json'
    Assert-Rejected (Join-Path $fixture 'virtualdesktop-openxr.json')
    Assert-Rejected (Join-Path $fixture 'unknown.json')
    $generic=Join-Path $fixture 'vendor-runtime.json'
    @{file_format_version='1.0.0';runtime=@{library_path='.\runtime.dll'}} | ConvertTo-Json | Set-Content -LiteralPath $generic
    Assert-Rejected $generic
    $resolved=Resolve-Grid2VrRuntime $generic -DefaultRuntime
    if($resolved.name -ne 'Default' -or $resolved.selection -ne 'Default' -or $resolved.library -ne $dll -or $resolved.theaterSuppression) { throw 'Default runtime validation mismatch.' }
    $env:XR_RUNTIME_JSON=$null;$script:systemManifest=$generic
    if((Get-Grid2VrRuntimeManifest 'Default') -ne $generic -or $script:systemReads -ne 1) { throw 'Default did not use the system runtime reader.' }
    $script:systemManifest=Join-Path $fixture 'steamxr_win32.json'
    $steam=Resolve-Grid2VrRuntime (Get-Grid2VrRuntimeManifest 'Default') -DefaultRuntime
    if($steam.selection -ne 'Default' -or $steam.name -ne 'SteamVR' -or !$steam.theaterSuppression) { throw 'Default SteamVR lost its theater handling.' }
    $script:systemManifest=$vdxr
    $desktop=Resolve-Grid2VrRuntime (Get-Grid2VrRuntimeManifest 'Default') -DefaultRuntime
    if($desktop.name -ne 'VDXR' -or $desktop.theaterSuppression) { throw 'Default VDXR gained SteamVR theater handling.' }
    $reads=$script:systemReads;$env:XR_RUNTIME_JSON=$generic
    if((Get-Grid2VrRuntimeManifest 'Default') -ne $generic -or $script:systemReads -ne $reads) { throw 'Process runtime override was not honored before the system setting.' }
    if([IO.Path]::GetFileName((Get-Grid2VrRuntimeManifest 'VDXR')) -ne 'virtualdesktop-openxr-32.json' -or
       [IO.Path]::GetFileName((Get-Grid2VrRuntimeManifest 'SteamVR')) -ne 'steamxr_win32.json') { throw 'Default override changed an explicit provider.' }
    $env:XR_RUNTIME_JSON=$null;$script:systemManifest=''
    $rejected=$false;try { Get-Grid2VrRuntimeManifest 'Default' | Out-Null } catch { $rejected=$_.Exception.Message -like '*No default 32-bit OpenXR runtime*' }
    if(!$rejected) { throw 'Missing default runtime did not give a clear error.' }
    Assert-Rejected 'vendor-runtime.json' -DefaultRuntime
    Assert-Rejected 'C:vendor-runtime.json' -DefaultRuntime
    Assert-Rejected (Join-Path $fixture 'missing.json') -DefaultRuntime
    Assert-Rejected (Join-Path $fixture 'runtime.dll') -DefaultRuntime
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes,68)
    [IO.File]::WriteAllBytes($dll,$bytes)
    Assert-Rejected $vdxr
    Assert-Rejected $generic -DefaultRuntime
    [BitConverter]::GetBytes([uint16]0x14c).CopyTo($bytes,68)
    [BitConverter]::GetBytes([uint16]0x20b).CopyTo($bytes,88)
    [IO.File]::WriteAllBytes($dll,$bytes)
    Assert-Rejected $vdxr
    [IO.File]::WriteAllBytes($dll,[byte[]]@(0,0))
    Assert-Rejected $vdxr
    Remove-Item -LiteralPath $dll
    Assert-Rejected $vdxr
    Write-Output 'Default discovery, process override, explicit VDXR/SteamVR, x86 validation and theater routing passed.'
} finally {
    $env:XR_RUNTIME_JSON=$originalOverride
    Set-Item Function:Get-Grid2VrSystemRuntimeManifest -Value $systemReader
    # This test creates only flat files in one fresh workspace directory.
    Get-ChildItem -LiteralPath $fixture -File | Remove-Item
    Remove-Item -LiteralPath $fixture
}

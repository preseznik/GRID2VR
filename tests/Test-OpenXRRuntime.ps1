$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\OpenXR-Runtime.ps1')
$fixture=Join-Path $root ('artifacts\runtime-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
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
    function Assert-Rejected([string]$path) {
        $rejected=$false
        try { Resolve-Grid2VrRuntime $path | Out-Null } catch { $rejected=$true }
        if(!$rejected) { throw "Unsafe runtime accepted: $path" }
    }
    Assert-Rejected 'virtualdesktop-openxr-32.json'
    Assert-Rejected (Join-Path $fixture 'virtualdesktop-openxr.json')
    Assert-Rejected (Join-Path $fixture 'unknown.json')
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes,68)
    [IO.File]::WriteAllBytes($dll,$bytes)
    Assert-Rejected $vdxr
    [BitConverter]::GetBytes([uint16]0x14c).CopyTo($bytes,68)
    [BitConverter]::GetBytes([uint16]0x20b).CopyTo($bytes,88)
    [IO.File]::WriteAllBytes($dll,$bytes)
    Assert-Rejected $vdxr
    [IO.File]::WriteAllBytes($dll,[byte[]]@(0,0))
    Assert-Rejected $vdxr
    Remove-Item -LiteralPath $dll
    Assert-Rejected $vdxr
    Write-Output 'VDXR/SteamVR selection, x86 PE validation, no-theater VDXR and wrong-runtime rejection passed.'
} finally {
    # This test creates only three flat files in one fresh workspace directory.
    Get-ChildItem -LiteralPath $fixture -File | Remove-Item
    Remove-Item -LiteralPath $fixture
}

# Skip video playback through the native Bink player. Keep checksum-covered
# startup definitions intact; Startup-Movies.ps1 remains for legacy recovery.
. (Join-Path $PSScriptRoot 'Startup-Movies.ps1')
$script:StartupBinkHash='03f9c695d4c45186d7b43ab64926fcb0933ca9a02eed43fd67cd8775ffac81e9'
$script:StartupFlowHash='8b2f7fce029ed2fe2e82d837d0e180f14347e14737f71566b0a318c46bd32894'

function Assert-NativeStartup([string]$Game) {
    $null=Get-Grid2ExecutableHash (Join-Path $Game 'grid2_avx.exe')
    Assert-StartupHash (Join-Path $Game 'bink2w32.dll') $script:StartupBinkHash
    Assert-StartupHash (Join-Path $Game 'system/states.bin') $script:StartupOriginalHash
    Assert-StartupHash (Join-Path $Game 'system/flow.bin') $script:StartupFlowHash
}

function Write-NativeStartupRequest([string]$Game,[string]$Run) {
    if(![IO.Path]::IsPathFullyQualified($Game) -or ![IO.Path]::IsPathFullyQualified($Run)) { throw 'Startup request paths must be absolute.' }
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    $runPath=[IO.Path]::GetFullPath($Run).TrimEnd('\')
    if($runPath -eq $gamePath -or $runPath.StartsWith($gamePath+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Startup request must be outside the game installation.' }
    Assert-StartupPlainPath $runPath
    if(!(Test-Path -LiteralPath $runPath -PathType Container)) { throw 'Startup request folder is missing.' }
    Assert-NativeStartup $gamePath
    $request=Join-Path $runPath 'startup-skip.request'
    Assert-StartupPlainPath $request
    $bytes=[Text.Encoding]::ASCII.GetBytes("GRID2VR_STARTUP_SKIP_V1`n")
    $stream=[IO.File]::Open($request,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    return @{strategy='native_movie_player';startupFilesChanged=$false}
}

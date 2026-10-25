$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Native-Startup.ps1')
$fixture=Join-Path $root ('build/native-startup-test-'+[Guid]::NewGuid().ToString('N'))
$game=Join-Path $fixture 'game'
$run=Join-Path $fixture 'run'
function Check($condition,$message) { if(!$condition) { throw $message } }
function Reject([scriptblock]$action) {
    $rejected=$false
    try { & $action | Out-Null } catch { $rejected=$true }
    Check $rejected 'Unsafe startup request accepted.'
}
try {
    New-Item -ItemType Directory -Path (Join-Path $game 'system'),$run | Out-Null
    $expected=@{}
    foreach($name in @('grid2_avx.exe','bink2w32.dll','system/states.bin','system/flow.bin')) {
        $path=Join-Path $game $name
        [IO.File]::WriteAllText($path,'fixture '+$name)
        $expected[$name]=(Get-FileHash -LiteralPath $path).Hash
    }
    $script:StartupExecutableHash=$expected['grid2_avx.exe']
    $script:StartupBinkHash=$expected['bink2w32.dll']
    $script:StartupOriginalHash=$expected['system/states.bin']
    $script:StartupFlowHash=$expected['system/flow.bin']
    foreach($name in $expected.Keys) {
        $path=Join-Path $game $name
        [IO.File]::WriteAllText($path,'changed')
        Reject { Write-NativeStartupRequest $game $run }
        Check (!(Test-Path -LiteralPath (Join-Path $run 'startup-skip.request'))) 'Rejected request wrote a marker.'
        [IO.File]::WriteAllText($path,'fixture '+$name)
    }
    Reject { Write-NativeStartupRequest $game $game }
    Reject { Write-NativeStartupRequest $game (Join-Path $game 'system') }
    Reject { Write-NativeStartupRequest $game 'relative' }
    $result=Write-NativeStartupRequest $game $run
    Check ($result.strategy -eq 'native_movie_player' -and !$result.startupFilesChanged) 'Wrong startup strategy.'
    $request=Join-Path $run 'startup-skip.request'
    Check ([IO.File]::ReadAllText($request) -ceq "GRID2VR_STARTUP_SKIP_V1`n") 'Request protocol changed.'
    [IO.File]::WriteAllText($request,'foreign request')
    Reject { Write-NativeStartupRequest $game $run }
    Check ([IO.File]::ReadAllText($request) -ceq 'foreign request') 'Existing request was overwritten.'
    foreach($name in $expected.Keys) {
        Check ((Get-FileHash -LiteralPath (Join-Path $game $name)).Hash -eq $expected[$name]) 'Startup preparation changed the game.'
    }
    'Native startup request: exact fingerprints, untouched game files and existing request protection passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    $boundary=[IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\')+'\'
    if(!$resolved.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path $resolved -Leaf) -notmatch '^native-startup-test-[0-9a-f]{32}$') { throw 'Unsafe fixture cleanup target.' }
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Headset-Settings.ps1')
$fixture=Join-Path $root ('build/headset-settings-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    foreach($scale in @(25,50,75,100,125,150,175,200)) {
        $settings=Get-HeadsetSettings $scale
        if($settings.width -ne 1280*$scale/100 -or $settings.height -ne 720*$scale/100 -or $settings.unitsPerMetre -ne 1 -or $settings.width*$settings.height -gt 4*1024*1024) { throw 'Invalid native image size.' }
        $path=Join-Path $fixture "$scale.request"
        Write-HeadsetRequest $path $scale
        if([IO.File]::ReadAllText($path) -ne $settings.request) { throw 'Native request serialization differs.' }
    }
    if([IO.File]::ReadAllText((Join-Path $fixture '100.request')) -cne '1280 720 1') { throw 'Accepted default request changed.' }
    foreach($scale in @(-1,0,24,26,99,201,400)) {
        $rejected=$false
        try { Write-HeadsetRequest (Join-Path $fixture 'invalid.request') $scale } catch { $rejected=$true }
        if(!$rejected -or (Test-Path -LiteralPath (Join-Path $fixture 'invalid.request'))) { throw 'Unsupported scale wrote a request.' }
    }
    $foreign=Join-Path $fixture 'foreign.request'
    [IO.File]::WriteAllText($foreign,'foreign content')
    $rejected=$false
    try { Write-HeadsetRequest $foreign 150 } catch { $rejected=$true }
    if(!$rejected -or [IO.File]::ReadAllText($foreign) -cne 'foreign content') { throw 'Foreign request overwritten.' }
    Write-Output 'Headset settings: all 8 sizes, unchanged default, invalid bounds and existing-file refusal passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($resolved) -ne (Join-Path $root 'build') -or [IO.Path]::GetFileName($resolved) -notmatch '^headset-settings-[0-9a-f]{32}$') { throw 'Fixture escaped workspace.' }
    if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
    Remove-Item -LiteralPath $resolved -Recurse
}

param([Parameter(Mandatory)][string]$GameRoot,[Parameter(Mandatory)][string]$ZipPath)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$fixture=[IO.Path]::GetFullPath((Join-Path $root ('build/cockpit-texture-install-test-'+[guid]::NewGuid().ToString('N'))))
$script=Join-Path $root 'tools/Cockpit-TexturePack.ps1'
$game=Join-Path $fixture 'game';$mod=Join-Path $game 'GRID2VR'
$target=Join-Path $mod 'addons/cockpit-textures/0.2.0-preview.2'
$tests=[Collections.Generic.List[string]]::new()
function Refuses([scriptblock]$Body,[string]$Name) {
    $failed=$false;try { & $Body | Out-Null } catch { $failed=$true }
    if(!$failed) { throw "Unexpected acceptance: $Name" };$tests.Add($Name)
}
try {
    [IO.Directory]::CreateDirectory($game)|Out-Null
    [IO.File]::Copy((Join-Path $GameRoot 'grid2_avx.exe'),(Join-Path $game 'grid2_avx.exe'))
    Refuses { & $script -Action Install -GameRoot $game -ZipPath $ZipPath } 'requires_core'
    [IO.Directory]::CreateDirectory($mod)|Out-Null;[IO.File]::WriteAllText((Join-Path $mod 'package.json'),'{}')
    $previous=Join-Path $mod 'addons/cockpit-textures/0.2.0-preview.1'
    [IO.Directory]::CreateDirectory($previous)|Out-Null
    $retained=Join-Path $previous 'retain.txt';[IO.File]::WriteAllText($retained,'previous pack retained')
    $badZip=Join-Path $fixture 'bad.zip';[IO.File]::WriteAllText($badZip,'invalid')
    Refuses { & $script -Action Install -GameRoot $game -ZipPath $badZip } 'rejects_foreign_archive'
    [IO.File]::WriteAllText((Join-Path $game 'GRID2VR.diagnostic-session'),'pending')
    Refuses { & $script -Action Install -GameRoot $game -ZipPath $ZipPath } 'refuses_pending_session'
    [IO.File]::Delete((Join-Path $game 'GRID2VR.diagnostic-session'))
    & $script -Action Install -GameRoot $game -ZipPath $ZipPath | Out-Null
    & $script -Action Verify -GameRoot $game | Out-Null
    $tests.Add('install_and_verify')
    Refuses { & $script -Action Install -GameRoot $game -ZipPath $ZipPath } 'existing_version_preserved'
    $extra=Join-Path $target 'foreign.txt';[IO.File]::WriteAllText($extra,'retain')
    Refuses { & $script -Action Remove -GameRoot $game } 'foreign_content_preserved'
    if([IO.File]::ReadAllText($extra) -cne 'retain' -or !(Test-Path -LiteralPath (Join-Path $target 'foc.dds'))) { throw 'Removal partially deleted foreign directory.' }
    [IO.File]::Delete($extra)
    $manifest=Join-Path $target 'pack.json';$original=[IO.File]::ReadAllBytes($manifest);[IO.File]::WriteAllText($manifest,'changed')
    Refuses { & $script -Action Remove -GameRoot $game } 'changed_pack_preserved'
    if(!(Test-Path -LiteralPath (Join-Path $target 'foc.dds'))) { throw 'Changed pack partially removed.' }
    [IO.File]::WriteAllBytes($manifest,$original)
    & $script -Action Remove -GameRoot $game | Out-Null
    if((Test-Path -LiteralPath $target) -or !(Test-Path -LiteralPath (Join-Path $mod 'package.json'))) { throw 'Unexpected removal scope.' }
    $tests.Add('remove_owned_version')
    if([IO.File]::ReadAllText($retained) -cne 'previous pack retained') { throw 'Previous pack was changed.' }
    $tests.Add('previous_preview_preserved_during_install_and_removal')
    $tests | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'build/cockpit-texture-install-tests.json')
    Write-Output "$($tests.Count) Cockpit texture pack install/removal checks passed in a disposable workspace game fixture."
} finally {
    $allowed=[IO.Path]::GetFullPath((Join-Path $root 'build')).TrimEnd('\')+'\'
    if(!$fixture.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path $fixture -Leaf) -notmatch '^cockpit-texture-install-test-[0-9a-f]{32}$') { throw 'Unsafe cleanup target.' }
    if(Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}

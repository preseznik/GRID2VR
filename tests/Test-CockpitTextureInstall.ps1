param([Parameter(Mandatory)][string]$GameRoot,[Parameter(Mandatory)][string]$ZipPath,
    # GRID2VR-CockpitTextures-0.2.0-preview.1.zip: a published earlier pack for the superseded-version cleanup checks.
    [Parameter(Mandatory)][string]$PreviousZipPath)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$fixture=[IO.Path]::GetFullPath((Join-Path $root ('build/cockpit-texture-install-test-'+[guid]::NewGuid().ToString('N'))))
$script=Join-Path $root 'tools/Cockpit-TexturePack.ps1'
$game=Join-Path $fixture 'game';$mod=Join-Path $game 'GRID2VR'
$target=Join-Path $mod 'addons/cockpit-textures/0.2.0-preview.3'
$tests=[Collections.Generic.List[string]]::new()
function Refuses([scriptblock]$Body,[string]$Name) {
    $failed=$false;try { & $Body | Out-Null } catch { $failed=$true }
    if(!$failed) { throw "Unexpected acceptance: $Name" };$tests.Add($Name)
}
$older=Join-Path $mod 'addons/cockpit-textures/0.2.0-preview.1'
# Lay out an earlier pack exactly as its installer did (flat files in its version folder).
function Install-Older {
    [IO.Directory]::CreateDirectory($older)|Out-Null
    $archive=[IO.Compression.ZipFile]::OpenRead($PreviousZipPath)
    try { foreach($entry in $archive.Entries) { if($entry.Name) { [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $older $entry.Name),$true) } } }
    finally { $archive.Dispose() }
}
try {
    [IO.Directory]::CreateDirectory($game)|Out-Null
    [IO.File]::Copy((Join-Path $GameRoot 'grid2_avx.exe'),(Join-Path $game 'grid2_avx.exe'))
    Refuses { & $script -Action Install -GameRoot $game -ZipPath $ZipPath } 'requires_core'
    [IO.Directory]::CreateDirectory($mod)|Out-Null;[IO.File]::WriteAllText((Join-Path $mod 'package.json'),'{}')
    $previous=Join-Path $mod 'addons/cockpit-textures/0.2.0-preview.2'
    [IO.Directory]::CreateDirectory($previous)|Out-Null
    $retained=Join-Path $previous 'retain.txt';[IO.File]::WriteAllText($retained,'previous pack retained')
    $badZip=Join-Path $fixture 'bad.zip';[IO.File]::WriteAllText($badZip,'invalid')
    Refuses { & $script -Action Install -GameRoot $game -ZipPath $badZip } 'rejects_foreign_archive'
    [IO.File]::WriteAllText((Join-Path $game 'GRID2VR.diagnostic-session'),'pending')
    Refuses { & $script -Action Install -GameRoot $game -ZipPath $ZipPath } 'refuses_pending_session'
    [IO.File]::Delete((Join-Path $game 'GRID2VR.diagnostic-session'))
    Install-Older
    $installed=& $script -Action Install -GameRoot $game -ZipPath $ZipPath
    if(Test-Path -LiteralPath $older) { throw 'A published earlier pack survived installing the new version.' }
    if(!($installed -match 'Removed older cockpit texture pack 0\.2\.0-preview\.1')) { throw 'Earlier pack removal was not reported.' }
    if(!($installed -match 'Kept older cockpit texture folder 0\.2\.0-preview\.2')) { throw 'Foreign earlier folder was not reported as kept.' }
    $tests.Add('install_removes_published_earlier_pack')
    & $script -Action Verify -GameRoot $game | Out-Null
    $tests.Add('install_and_verify')
    # Verify also tidies a published earlier pack, but only when nothing is running.
    Install-Older
    [IO.File]::WriteAllText((Join-Path $game 'GRID2VR.diagnostic-session'),'pending')
    & $script -Action Verify -GameRoot $game | Out-Null
    if(!(Test-Path -LiteralPath (Join-Path $older 'foc.dds'))) { throw 'Earlier pack removed while a session was pending.' }
    $tests.Add('verify_keeps_earlier_pack_while_session_pending')
    [IO.File]::Delete((Join-Path $game 'GRID2VR.diagnostic-session'))
    $changed=Join-Path $older 'foc.dds';$changedBytes=[IO.File]::ReadAllBytes($changed);[IO.File]::WriteAllText($changed,'changed')
    & $script -Action Verify -GameRoot $game | Out-Null
    if([IO.File]::ReadAllText($changed) -cne 'changed' -or @(Get-ChildItem -LiteralPath $older).Count -ne 74) { throw 'A changed earlier pack was altered.' }
    $tests.Add('changed_earlier_pack_kept')
    [IO.File]::WriteAllBytes($changed,$changedBytes)
    & $script -Action Verify -GameRoot $game | Out-Null
    if(Test-Path -LiteralPath $older) { throw 'Verify did not remove the published earlier pack.' }
    $tests.Add('verify_removes_published_earlier_pack')
    $reference=Join-Path $target 'CARS.txt'
    $text=[IO.File]::ReadAllText($reference)
    if($text -notmatch 'Ford Focus ST\s+\| foc\s+\| foc.dds' -or $text -notmatch 'Volkswagen Golf R \(Performance\)\s+\| go2\s+\| gol.dds') { throw 'Readable car reference is missing or incomplete.' }
    $tests.Add('readable_names_and_shared_texture_installed')
    $referenceBytes=[IO.File]::ReadAllBytes($reference);[IO.File]::WriteAllText($reference,'changed')
    Refuses { & $script -Action Verify -GameRoot $game } 'changed_car_reference_rejected'
    [IO.File]::WriteAllBytes($reference,$referenceBytes)
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

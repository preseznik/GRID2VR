$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Fake-Vr.ps1')
function Check([bool]$Value,[string]$Message) { if(!$Value) { throw $Message } }
$fixture=Join-Path $root ('build/fake-vr-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    # SteamVR's own file: unrelated sections survive, the null driver is forced and standby is off.
    $path=Join-Path $fixture 'steamvr.vrsettings'
    Set-Content -LiteralPath $path -Value '{"DesktopUI":{"pairing":"1,2,3,4,0"},"steamvr":{"forcedDriver":"","mirrorViewGeometry":"0 0 1 1"}}' -NoNewline
    Set-FakeVrNullDriver $path 1600 1800
    $settings=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
    Check ($settings['steamvr']['forcedDriver'] -ceq 'null' -and $settings['steamvr']['activateMultipleDrivers'] -eq $true) 'Null driver not forced.'
    Check ($settings['steamvr']['mirrorViewGeometry'] -ceq '0 0 1 1' -and $settings['DesktopUI']['pairing'] -ceq '1,2,3,4,0') 'Unrelated SteamVR settings changed.'
    Check ($settings['power']['turnOffScreensTimeout'] -eq 86400 -and $settings['power']['pauseCompositorOnStandby'] -eq $false) 'Standby still pauses the compositor.'
    Check ($settings['driver_null']['renderWidth'] -eq 1600 -and $settings['driver_null']['renderHeight'] -eq 1800 -and $settings['driver_null']['enable'] -eq $true) 'Virtual headset size not applied.'
    # A minimal file without steamvr or power sections still gets both.
    Set-Content -LiteralPath $path -Value '{}' -NoNewline
    Set-FakeVrNullDriver $path
    $settings=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
    Check ($settings['steamvr']['forcedDriver'] -ceq 'null' -and $settings['driver_null']['renderWidth'] -eq 2016) 'Minimal settings not completed.'
    # Only a renderer with the zero-adapter fix is accepted for a virtual-headset run.
    $renderer=Join-Path $fixture 'd3d11.dll'
    [IO.File]::WriteAllBytes($renderer,[Text.Encoding]::ASCII.GetBytes("MZ..runtime names no graphics adapter; using the game's.."))
    Check (Test-FakeVrRenderer $renderer) 'Renderer with the adapter fix refused.'
    [IO.File]::WriteAllBytes($renderer,[Text.Encoding]::ASCII.GetBytes('MZ..device game=%p compatible=%u..'))
    Check (!(Test-FakeVrRenderer $renderer) -and !(Test-FakeVrRenderer (Join-Path $fixture 'missing.dll'))) 'Renderer without the adapter fix accepted.'
    Write-Output 'Fake VR: null-driver settings, unrelated settings kept, standby off, renderer gate passed.'
} finally { Remove-Item -LiteralPath $fixture -Recurse -Force }

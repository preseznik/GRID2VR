param([ValidateSet('VDXR','SteamVR')][string]$Runtime='VDXR')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Headset-Resolution.ps1')
try { Get-ConnectedHeadsetResolution (Split-Path $PSScriptRoot -Parent) $Runtime | ConvertTo-Json -Compress }
catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }

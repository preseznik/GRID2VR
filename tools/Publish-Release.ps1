[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageDirectory,[switch]$Check)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
$python=Get-Grid2Python $root
$arguments=@((Join-Path $PSScriptRoot 'publish_release.py'),'--package',[IO.Path]::GetFullPath($PackageDirectory))
if($Check) { $arguments+='--check' }
& $python @arguments
if($LASTEXITCODE) { throw 'Release publication stopped. Existing release assets were retained; inspect the error before retrying.' }

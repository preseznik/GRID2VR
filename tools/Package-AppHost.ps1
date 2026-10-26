# Build-time only: create a standard .NET apphost that uses the packaged runtime.
param([Parameter(Mandatory)][string]$Output,[Parameter(Mandatory)][string]$Assembly,
    [Parameter(Mandatory)][string]$Resources,[string]$RuntimeVersion='10.0.13')
$ErrorActionPreference='Stop'
$dotnet=Split-Path (Get-Command dotnet -ErrorAction Stop).Source -Parent
$sdk=(& dotnet --version).Trim()
if($LASTEXITCODE) { throw 'Could not identify the .NET SDK.' }
Add-Type -Path (Join-Path $dotnet "sdk/$sdk/Microsoft.NET.HostModel.dll")
$template=Join-Path $dotnet "packs/Microsoft.NETCore.App.Host.win-x64/$RuntimeVersion/runtimes/win-x64/native/apphost.exe"
$search=[Microsoft.NET.HostModel.AppHost.HostWriter+DotNetSearchOptions]::new()
$search.Location=[Microsoft.NET.HostModel.AppHost.HostWriter+DotNetSearchOptions+SearchLocation]::AppRelative
$search.AppRelativeDotNet='../dotnet'
[Microsoft.NET.HostModel.AppHost.HostWriter]::CreateAppHost($template,$Output,$Assembly,$false,$Resources,$false,$false,$search)

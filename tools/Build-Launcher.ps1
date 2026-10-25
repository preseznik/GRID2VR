param([string]$OutputDirectory='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$OutputDirectory) { $OutputDirectory=Join-Path $root 'build/launcher' }
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$cmake=(Get-Command cmake -ErrorAction Stop).Source
$inputBuild=Join-Path $root 'build/driving-input-vs-x64'
& $cmake -S (Join-Path $root 'tools/driving-input') -B $inputBuild -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE) { throw 'Controller capture configuration failed.' }
& $cmake --build $inputBuild --config Release --parallel 4
if($LASTEXITCODE) { throw 'Controller capture build failed.' }
& dotnet build (Join-Path $root 'launcher/GRID2VR.csproj') -c Release --nologo -o $OutputDirectory -p:NuGetAudit=false
if($LASTEXITCODE) { throw 'Launcher build failed.' }
Copy-Item -LiteralPath (Join-Path $inputBuild 'Release/driving_input.dll') -Destination (Join-Path $OutputDirectory 'driving_input.dll') -Force
Write-Output ('Launcher: '+(Join-Path $OutputDirectory 'GRID2VR.exe'))

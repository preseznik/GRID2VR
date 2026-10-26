[CmdletBinding()]
param([ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version='0.1.9',[ValidateSet('Stable','Experimental')][string]$Channel='Stable',
    [string]$InnoCompiler="$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe")
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Paths.ps1')
$python=Get-Grid2Python $root
$output=Join-Path $root ('artifacts/packages/'+$Version+'-'+$Channel+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output | Out-Null
$publish=Join-Path $output 'publish'
$revision=(& git -c "safe.directory=$($root.Replace('\','/'))" -C $root rev-parse --short=12 HEAD).Trim()
$changes=& git -c "safe.directory=$($root.Replace('\','/'))" -C $root status --porcelain
if($LASTEXITCODE) { throw 'Could not determine release source state.' }
$buildRevision=if($changes) { "$revision-local" } else { $revision }
$built=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
& dotnet publish (Join-Path $root 'launcher/GRID2VR.csproj') -c Release -r win-x64 --self-contained false -o $publish --nologo -p:PublishSingleFile=true -p:PublishTrimmed=false -p:RuntimeFrameworkVersion=10.0.11 -p:AppHostDotNetSearch=AppRelative -p:AppHostRelativeDotNet=GRID2VR/payload/dotnet -p:NuGetAudit=false "-p:Version=$Version" "-p:ReleaseChannel=$Channel" "-p:BuildUtc=$built" "-p:BuildRevision=$buildRevision"
if($LASTEXITCODE) { throw 'Private-runtime launcher publish failed.' }
& $python (Join-Path $root 'tools/package_stage.py') --output $output --python $python --version $Version --channel $Channel --revision $revision --built $built
if($LASTEXITCODE) { throw 'Package staging failed.' }
& $InnoCompiler "/DStage=$(Join-Path $output 'stage')" "/DPackageVersion=$Version" (Join-Path $root 'installer/GRID2VR.iss')
if($LASTEXITCODE) { throw 'Installer compilation failed.' }
& $python (Join-Path $root 'tools/package_stage.py') --finalize $output
if($LASTEXITCODE) { throw 'Package finalization failed.' }
Write-Output "Package: $output"

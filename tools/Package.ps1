[CmdletBinding()]
param([ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version='0.2.1',[ValidateSet('Stable','Experimental')][string]$Channel='Stable',
    [string]$InnoCompiler="$env:LOCALAPPDATA\Programs\Inno Setup 7\ISCC.exe",[string]$TexturePack='',
    [string]$NativeBinary='',[string]$NativeSha256='',[switch]$AllowPinnedNative,[switch]$Publish)
$ErrorActionPreference='Stop'
if([bool]$NativeBinary -ne [bool]$NativeSha256) { throw 'NativeBinary and NativeSha256 must be supplied together.' }
if($AllowPinnedNative -and !$NativeBinary) { throw 'AllowPinnedNative applies only to a supplied NativeBinary.' }
if($Publish -and $NativeBinary -and !$AllowPinnedNative) { throw 'Publishing a supplied NativeBinary requires -AllowPinnedNative. Omit -NativeBinary to build the renderer from the released commit.' }
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Paths.ps1')
$python=Get-Grid2Python $root
$output=Join-Path $root ('artifacts/packages/'+$Version+'-'+$Channel+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output | Out-Null
$launcherOutput=Join-Path $output 'publish'
$revision=(& git -c "safe.directory=$($root.Replace('\','/'))" -C $root rev-parse --short=12 HEAD).Trim()
$commit=(& git -c "safe.directory=$($root.Replace('\','/'))" -C $root rev-parse HEAD).Trim()
$changes=& git -c "safe.directory=$($root.Replace('\','/'))" -C $root status --porcelain
if($LASTEXITCODE) { throw 'Could not determine release source state.' }
$buildRevision=if($changes) { "$revision-local" } else { $revision }
if($Publish -and $changes) { throw 'Commit all changes before publishing; a public release is built from its commit.' }
$built=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$packArguments=@((Join-Path $root 'tools/release_assets.py'),'--output',$output,'--version',$Version,'--channel',$Channel,'--revision',$commit)
if($TexturePack) { $packArguments+=@('--pack',[IO.Path]::GetFullPath($TexturePack)) }
if($NativeBinary) {
    if($AllowPinnedNative) { $packArguments+='--allow-pinned-publish' }
} else {
    # Every committed renderer fix ships: build the renderer, XR probe and input
    # DLL from this source with release settings. A dirty tree is marked -dirty
    # in the renderer and cannot be published.
    $nativeRevision=if($changes) { "$commit-dirty" } else { $commit }
    $cmake=(Get-Command cmake -ErrorAction Stop).Source
    $nativeBuild=Join-Path $root 'build/release-native'
    & $cmake -S $root -B $nativeBuild -G 'Visual Studio 17 2022' -A Win32 -DGRID2VR_ENABLE_XR_SESSION=ON -DGRID2VR_BUILD_XR_PROBE=ON `
        -DGRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION=ON -DGRID2VR_OPPONENT_LOD_SINGLE_ATTEMPT_DIAGNOSTIC=OFF `
        "-DGRID2VR_SOURCE_REVISION=$nativeRevision" "-DPython3_EXECUTABLE=$python"
    if($LASTEXITCODE) { throw 'Release renderer configuration failed.' }
    & $cmake --build $nativeBuild --config Release --target d3d11 xr_runtime_probe --parallel 8
    if($LASTEXITCODE) { throw 'Release renderer build failed.' }
    $inputBuild=Join-Path $root 'build/driving-input-vs-x64'
    & $cmake -S (Join-Path $root 'tools/driving-input') -B $inputBuild -G 'Visual Studio 17 2022' -A x64
    if($LASTEXITCODE) { throw 'Controller capture configuration failed.' }
    & $cmake --build $inputBuild --config Release --parallel 4
    if($LASTEXITCODE) { throw 'Controller capture build failed.' }
    $NativeBinary=Join-Path $nativeBuild 'bin/Release/d3d11.dll'
    $NativeSha256=(Get-FileHash -LiteralPath $NativeBinary).Hash.ToLowerInvariant()
    $packArguments+=@('--native-revision',$nativeRevision)
}
$nativeArguments=@('--native',[IO.Path]::GetFullPath($NativeBinary))
$packArguments+=$nativeArguments+@('--native-sha256',$NativeSha256.ToLowerInvariant())
& $python @packArguments
if($LASTEXITCODE) { throw 'The matching cockpit texture ZIP is required before building a release.' }
& dotnet publish (Join-Path $root 'launcher/GRID2VR.csproj') -c Release -r win-x64 --self-contained false -o $launcherOutput --nologo -p:PublishSingleFile=true -p:PublishTrimmed=false -p:RuntimeFrameworkVersion=10.0.11 -p:AppHostDotNetSearch=AppRelative -p:AppHostRelativeDotNet=GRID2VR/payload/dotnet -p:NuGetAudit=false "-p:Version=$Version" "-p:ReleaseChannel=$Channel" "-p:BuildUtc=$built" "-p:BuildRevision=$buildRevision"
if($LASTEXITCODE) { throw 'Private-runtime launcher publish failed.' }
if([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $launcherOutput 'GRID2VR.exe')).ProductVersion.Split('+')[0] -ne $Version) { throw 'Built launcher version differs from the release download URL.' }
& $python (Join-Path $root 'tools/package_stage.py') --output $output --python $python --version $Version --channel $Channel --revision $revision --built $built @nativeArguments
if($LASTEXITCODE) { throw 'Package staging failed.' }
& $InnoCompiler "/DStage=$(Join-Path $output 'stage')" "/DPackageVersion=$Version" (Join-Path $root 'installer/GRID2VR.iss')
if($LASTEXITCODE) { throw 'Installer compilation failed.' }
& $python (Join-Path $root 'tools/package_stage.py') --finalize $output
if($LASTEXITCODE) { throw 'Package finalization failed.' }
Write-Output "Package: $output"
if($Publish) { & (Join-Path $PSScriptRoot 'Publish-Release.ps1') -PackageDirectory $output }

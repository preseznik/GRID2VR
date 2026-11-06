$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/Headset-Resolution.ps1')
function Assert($Value,[string]$Message) { if(!$Value) { throw $Message } }
function Reject([scriptblock]$Action) { $rejected=$false;try { & $Action | Out-Null } catch { $rejected=$true };Assert $rejected 'Invalid resolution was accepted' }
$query="runtime=VirtualDesktopXR version=1.0.0`nheadset=Fixture headset orientation_tracking=1 position_tracking=1`neye0=2400x2600 recommended_samples=1 max=8192x8192`neye1=2400x2600 recommended_samples=1 max=8192x8192`n"
$headset=ConvertFrom-HeadsetQuery $query 'VDXR'
Assert ($headset.width -eq 2400 -and $headset.height -eq 2600 -and $headset.headset -eq 'Fixture headset') 'Query parsing'
foreach($scale in 50,100,137,150,200,300) {
    $plan=Get-HeadsetResolutionPlan $headset.width $headset.height $scale
    Assert ($plan.width -eq 2400 -and $plan.height -eq 2600 -and $plan.sceneWidth -eq [Math]::Floor(2400*$scale/100) -and $plan.sceneHeight -eq [Math]::Floor(2600*$scale/100)) 'Output must stay fixed while scene scales'
}
foreach($dims in @(@(0,2600,100),@(8193,2600,100),@(5000,5000,50),@(3072,3264,252),@(2400,2600,49),@(2400,2600,301))) {
    Reject { Get-HeadsetResolutionPlan $dims[0] $dims[1] $dims[2] }
}
Reject { ConvertFrom-HeadsetQuery $query 'SteamVR' }
Reject { ConvertFrom-HeadsetQuery ($query.Replace('eye1=2400','eye1=2500')) 'VDXR' }
Reject { ConvertFrom-HeadsetQuery ($query.Replace('max=8192x8192','max=2000x2000')) 'VDXR' }
Reject { ConvertFrom-HeadsetQuery 'get_headset_system=-34' 'VDXR' }
Assert ((ConvertFrom-HeadsetQuery ($query.Replace('VirtualDesktopXR','SteamVR/test')) 'SteamVR').width -eq 2400) 'SteamVR query'
foreach($provider in 'VirtualDesktopXR','SteamVR/test','VendorOpenXR') {
    $system=ConvertFrom-HeadsetQuery ($query.Replace('VirtualDesktopXR',$provider)) 'Default'
    Assert ($system.runtime -eq 'Default' -and $system.runtimeName -ceq $provider) 'Default runtime keeps the requested selection and reports the actual provider'
}
Reject { ConvertFrom-HeadsetQuery $query 'Default' 'SteamVR' }
Reject { ConvertFrom-HeadsetQuery ($query.Replace('VirtualDesktopXR','VendorOpenXR')) 'Default' 'VDXR' }
$unavailable=[pscustomobject]@{runtime='Default';exitCode=1;output='get_headset_system=-35';errors=''}
Assert ((Get-HeadsetQueryFailure $unavailable).Contains('default OpenXR runtime')) 'Default without a headset must explain how to connect or select a runtime'
foreach($dimensions in @(@(150,4608,4896),@(200,6144,6528),@(251,7710,8192))) {
    $quest=Get-HeadsetResolutionPlan 3072 3264 $dimensions[0]
    Assert ($quest.sceneWidth -eq $dimensions[1] -and $quest.sceneHeight -eq $dimensions[2] -and $quest.width -eq 3072 -and $quest.height -eq 3264) 'Quest 3 supersampling and fixed output'
}
Reject { Get-HeadsetResolutionPlan 3072 3264 252 }
# Field of view crops evenly: output stays fixed, the scene shrinks by the same factor.
$cropped=Get-HeadsetResolutionPlan 3072 3264 150 80
Assert ($cropped.sceneWidth -eq 3686 -and $cropped.sceneHeight -eq 3916 -and $cropped.width -eq 3072 -and $cropped.height -eq 3264 -and $cropped.fov -eq 80 -and $cropped.resolutionRequest -ceq '3072 3264 150 80') 'Cropped field of view'
Assert ((Get-HeadsetResolutionPlan 3072 3264 150 100).resolutionRequest -ceq '3072 3264 150') 'Full field of view keeps the three-value request'
Assert ((Get-HeadsetResolutionPlan 3072 3264 300 80).sceneWidth -eq 7372) 'A cropped field of view leaves room for a higher render scale'
foreach($fov in 9,101) { Reject { Get-HeadsetResolutionPlan 3072 3264 100 $fov } }
# A helmet crop trims only top and bottom: width keeps the full field of view.
$helmet=Get-HeadsetResolutionPlan 3072 3264 150 80 Vertical
Assert ($helmet.sceneWidth -eq 4608 -and $helmet.sceneHeight -eq 3916 -and $helmet.fovCrop -ceq 'Vertical' -and $helmet.resolutionRequest -ceq '3072 3264 150 100 80') 'Helmet crop'
Assert ((Get-HeadsetResolutionPlan 3072 3264 150 100 Vertical).resolutionRequest -ceq '3072 3264 150') 'A full helmet view keeps the three-value request'
Reject { Get-HeadsetResolutionPlan 3072 3264 150 80 Sideways }
# Both crops go down to 10%, as long as the scene stays above the renderer minimum (320 x 180).
$deep=Get-HeadsetResolutionPlan 3072 3264 150 50 Vertical
Assert ($deep.sceneWidth -eq 4608 -and $deep.sceneHeight -eq 2448 -and $deep.resolutionRequest -ceq '3072 3264 150 100 50') 'A helmet crop reaches 50% vertically'
$helmet10=Get-HeadsetResolutionPlan 3072 3264 100 10 Vertical
Assert ($helmet10.sceneWidth -eq 3072 -and $helmet10.sceneHeight -eq 326 -and $helmet10.resolutionRequest -ceq '3072 3264 100 100 10') 'A helmet crop reaches 10% vertically'
foreach($fov in 9,101) { Reject { Get-HeadsetResolutionPlan 3072 3264 150 $fov Vertical } }
$even=Get-HeadsetResolutionPlan 3072 3264 150 50
Assert ($even.sceneWidth -eq 2304 -and $even.sceneHeight -eq 2448 -and $even.resolutionRequest -ceq '3072 3264 150 50') 'An even crop reaches 50%'
$even15=Get-HeadsetResolutionPlan 3072 3264 100 15
Assert ($even15.sceneWidth -eq 460 -and $even15.sceneHeight -eq 489 -and $even15.resolutionRequest -ceq '3072 3264 100 15') 'An even crop reaches 15%'
Reject { Get-HeadsetResolutionPlan 3072 3264 100 10 }
Reject { Get-HeadsetResolutionPlan 640 640 50 70 }
$fixture=Join-Path (Split-Path $PSScriptRoot -Parent) ('build/headset-resolution-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    Write-HeadsetResolutionRequests $fixture 2400 2600 137
    Assert ([IO.File]::ReadAllText((Join-Path $fixture 'xr-projection.request')) -ceq '2400 2600 1') 'Native output request'
    Assert ([IO.File]::ReadAllText((Join-Path $fixture 'headset-resolution.request')) -ceq '2400 2600 137') 'Native recommendation verification request'
    Reject { Write-HeadsetResolutionRequests $fixture 2000 2000 100 }
    Assert ([IO.File]::ReadAllText((Join-Path $fixture 'xr-projection.request')) -ceq '2400 2600 1') 'Existing request was overwritten'
    $proxy=Join-Path $fixture 'renderer-fixture.dll'
    [IO.File]::WriteAllText($proxy,'old renderer')
    Reject { Assert-HeadsetResolutionProxy $proxy }
    [IO.File]::WriteAllText($proxy,'headset_resolution verified=1')
    Assert-HeadsetResolutionProxy $proxy
    foreach($name in 'xr-projection.request','headset-resolution.request') { Remove-Item -LiteralPath (Join-Path $fixture $name) }
    Write-HeadsetResolutionRequests $fixture 2400 2600 137 85
    Assert ([IO.File]::ReadAllText((Join-Path $fixture 'xr-projection.request')) -ceq '2400 2600 1') 'Cropped native output request'
    Assert ([IO.File]::ReadAllText((Join-Path $fixture 'headset-resolution.request')) -ceq '2400 2600 137 85') 'Cropped verification request'
    foreach($name in 'xr-projection.request','headset-resolution.request') { Remove-Item -LiteralPath (Join-Path $fixture $name) }
    Write-HeadsetResolutionRequests $fixture 2400 2600 137 85 Vertical
    Assert ([IO.File]::ReadAllText((Join-Path $fixture 'headset-resolution.request')) -ceq '2400 2600 137 100 85') 'Helmet verification request'
} finally {
    # Only the three files created above; no recursive deletion or linked trees.
    foreach($name in 'xr-projection.request','headset-resolution.request','renderer-fixture.dll') {
        $path=Join-Path $fixture $name
        if(Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    Remove-Item -LiteralPath $fixture
}
Write-Output 'Headset resolution: runtime identity, both eyes, fixed output, scene scaling, field of view and bounds passed.'

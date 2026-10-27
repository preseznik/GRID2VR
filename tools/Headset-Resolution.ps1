# One scene scale relative to the connected headset. Output never follows it.
function Assert-HeadsetResolutionProxy([string]$Path) {
    $file=Get-Item -LiteralPath $Path -ErrorAction Stop
    if($file.Length -gt 32MB -or ![Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($file.FullName)).Contains('headset_resolution verified=1')) {
        throw 'This renderer does not support headset-relative resolution. Update GRID2VR; in a development workspace, turn off Preview VR fixes and rebuild the current renderer.'
    }
}

function Get-HeadsetResolutionPlan([int]$Width,[int]$Height,[int]$Scale=100) {
    if($Width -le 0 -or $Height -le 0 -or $Width -gt 8192 -or $Height -gt 8192 -or [long]$Width*$Height -gt 16MB) {
        throw 'The headset recommended resolution exceeds the renderer limit (8192 per dimension, 16,777,216 pixels per eye).'
    }
    if($Scale -lt 50 -or $Scale -gt 300) { throw 'VR render scale must be 50-300%.' }
    $sceneWidth=[int][Math]::Floor($Width*$Scale/100.0)
    $sceneHeight=[int][Math]::Floor($Height*$Scale/100.0)
    if($sceneWidth -lt 320 -or $sceneHeight -lt 180 -or $sceneWidth -gt 8192 -or $sceneHeight -gt 8192 -or [long]$sceneWidth*$sceneHeight -gt 64MB) {
        $maximum=300
        while($maximum -ge 50) {
            $w=[long][Math]::Floor($Width*$maximum/100.0); $h=[long][Math]::Floor($Height*$maximum/100.0)
            if($w -le 8192 -and $h -le 8192 -and $w*$h -le 64MB) { break }
            $maximum--
        }
        throw "VR render scale $Scale% requests $sceneWidth x $sceneHeight per eye, exceeding renderer limits. Maximum at this headset resolution: $maximum%. Lower VR render scale."
    }
    return [ordered]@{width=$Width;height=$Height;scale=$Scale;sceneWidth=$sceneWidth;sceneHeight=$sceneHeight;unitsPerMetre=1;request="$Width $Height 1";basis='connected headset OpenXR recommendation'}
}

function Write-HeadsetResolutionRequests([string]$Run,[int]$Width,[int]$Height,[int]$Scale) {
    $plan=Get-HeadsetResolutionPlan $Width $Height $Scale
    foreach($entry in @(@('xr-projection.request',$plan.request),@('headset-resolution.request',"$Width $Height $Scale"))) {
        $stream=[IO.File]::Open((Join-Path $Run $entry[0]),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        try {
            $bytes=[Text.Encoding]::ASCII.GetBytes($entry[1]);$stream.Write($bytes,0,$bytes.Length)
        } finally { $stream.Dispose() }
    }
}

function ConvertFrom-HeadsetQuery([string]$Text,[string]$Runtime) {
    if($Text.Length -gt 65536) { throw 'Headset query returned too much data.' }
    $identity=[regex]::Match($Text,'(?m)^runtime=(.+) version=\d+\.\d+\.\d+\r?$')
    if(!$identity.Success -or ($Runtime -eq 'VDXR' -and $identity.Groups[1].Value -cne 'VirtualDesktopXR') -or
       ($Runtime -eq 'SteamVR' -and !$identity.Groups[1].Value.StartsWith('SteamVR/',[StringComparison]::Ordinal))) { throw 'Headset query returned an unexpected OpenXR runtime.' }
    $eyes=[regex]::Matches($Text,'(?m)^eye([01])=(\d+)x(\d+) recommended_samples=\d+ max=(\d+)x(\d+)\r?$')
    if($eyes.Count -ne 2 -or $eyes[0].Groups[1].Value -ne '0' -or $eyes[1].Groups[1].Value -ne '1') { throw 'Could not read both headset eye resolutions.' }
    $width=[int]$eyes[0].Groups[2].Value; $height=[int]$eyes[0].Groups[3].Value
    foreach($eye in $eyes) {
        if([int]$eye.Groups[2].Value -ne $width -or [int]$eye.Groups[3].Value -ne $height) { throw 'This renderer requires matching recommended dimensions for both eyes.' }
        if([int]$eye.Groups[4].Value -lt $width -or [int]$eye.Groups[5].Value -lt $height) { throw 'Headset recommendation exceeds its OpenXR image limits.' }
    }
    $plan=Get-HeadsetResolutionPlan $width $height
    $name=[regex]::Match($Text,'(?m)^headset=(.+) orientation_tracking=\d+ position_tracking=\d+\r?$')
    if(!$name.Success) { throw 'Headset query did not identify a headset.' }
    return [ordered]@{runtime=$Runtime;headset=$name.Groups[1].Value;width=$plan.width;height=$plan.height}
}

function Get-ConnectedHeadsetResolution([string]$Root,[ValidateSet('VDXR','SteamVR')][string]$Runtime) {
    . (Join-Path $PSScriptRoot 'Paths.ps1')
    . (Join-Path $PSScriptRoot 'OpenXR-Runtime.ps1')
    $manifest=Get-Grid2VrRuntimeManifest $Runtime
    $null=Resolve-Grid2VrRuntime $manifest
    $probe=if(Test-Grid2Package $Root) { Get-Grid2PackageFile $Root 'payload/xr_runtime_probe.exe' } else { Join-Path $Root 'build/win32/bin/RelWithDebInfo/xr_runtime_probe.exe' }
    if(!(Test-Path -LiteralPath $probe -PathType Leaf)) { throw 'Headset resolution helper is missing. Rebuild the native renderer or reinstall GRID2VR.' }
    $start=[Diagnostics.ProcessStartInfo]::new($probe)
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
    $start.Environment['XR_RUNTIME_JSON']=$manifest
    $process=[Diagnostics.Process]::Start($start)
    try {
        $output=$process.StandardOutput.ReadToEndAsync(); $errorOutput=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(15000)) {
            $process.Kill($true)
            throw "Headset query timed out. Connect your headset through $Runtime and refresh."
        }
        $text=$output.GetAwaiter().GetResult(); $null=$errorOutput.GetAwaiter().GetResult()
        if($process.ExitCode -ne 0) { throw "Could not read the headset resolution (query exit $($process.ExitCode)). Connect your headset through $Runtime and refresh." }
        return ConvertFrom-HeadsetQuery $text $Runtime
    } finally { $process.Dispose() }
}

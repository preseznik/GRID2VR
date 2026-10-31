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

. (Join-Path $PSScriptRoot 'OpenXR-Layers.ps1')

# Run the 32-bit probe once. $LayerBypasses are approved, revalidated layer identities.
function Invoke-HeadsetQuery([string]$Root,[ValidateSet('VDXR','SteamVR')][string]$Runtime,[object[]]$LayerBypasses=@()) {
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
    # Loader warnings name an add-on layer that failed to load; keep a user's more verbose choice.
    if(!$start.Environment.ContainsKey('XR_LOADER_DEBUG') -or $start.Environment['XR_LOADER_DEBUG'] -notin 'warn','info','verbose','all') { $start.Environment['XR_LOADER_DEBUG']='warn' }
    foreach($layer in @($LayerBypasses | Where-Object { $_ })) {
        if(!(Test-Grid2VrLayerDisableVariable $layer.disableVariable)) { throw 'Invalid OpenXR layer disable switch.' }
        $start.Environment[$layer.disableVariable]='1'
    }
    $process=[Diagnostics.Process]::Start($start)
    try {
        $output=$process.StandardOutput.ReadToEndAsync(); $errorOutput=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(15000)) {
            $process.Kill($true)
            throw "Headset query timed out. Connect your headset through $Runtime and refresh."
        }
        $text=$output.GetAwaiter().GetResult(); $errors=$errorOutput.GetAwaiter().GetResult()
        if($errors.Length -gt 65536) { $errors=$errors.Substring(0,65536) }
        return [pscustomobject][ordered]@{runtime=$Runtime;exitCode=$process.ExitCode;output=$text;errors=$errors
            disabledLayers=@($LayerBypasses | Where-Object { $_ } | ForEach-Object { $_.layerName })}
    } finally { $process.Dispose() }
}

# Loader warnings share stdout with the probe; loader errors also go to stderr.
function Get-HeadsetQueryText($Query) { return "$($Query.output)`n$($Query.errors)" }

function Get-HeadsetQueryReport($Query) {
    return "runtime_selection=$($Query.runtime)`r`ndisabled_api_layers=$($Query.disabledLayers -join ',')`r`nexit_code=$($Query.exitCode)`r`n$(Get-HeadsetQueryText $Query)"
}

function Get-HeadsetQueryFailure($Query) {
    $failures=@(Get-Grid2VrLayerFailures $Query.exitCode (Get-HeadsetQueryText $Query))
    if($failures.Count) { return Format-Grid2VrLayerFailures $failures }
    $step=[regex]::Matches($Query.output,'(?m)^([a-z_]+)=(-[0-9]+)\r?$') | Select-Object -Last 1
    $detail=if($step) { ", $($step.Groups[1].Value)=$($step.Groups[2].Value)" } else { '' }
    return "Could not read the headset resolution (query exit $($Query.exitCode)$detail). Connect your headset through $($Query.runtime) and refresh."
}

function Get-ConnectedHeadsetResolution([string]$Root,[ValidateSet('VDXR','SteamVR')][string]$Runtime,[object[]]$LayerBypasses=@(),[string]$ReportPath='',[switch]$ReportSuccess) {
    $query=Invoke-HeadsetQuery $Root $Runtime $LayerBypasses
    # A failed query always keeps its loader report; it names the step and any failing layer.
    if($ReportPath -and ($query.exitCode -ne 0 -or $ReportSuccess)) { [IO.File]::WriteAllText($ReportPath,(Get-HeadsetQueryReport $query),[Text.UTF8Encoding]::new($false)) }
    if($query.exitCode -ne 0) { throw (Get-HeadsetQueryFailure $query) }
    return ConvertFrom-HeadsetQuery $query.output $Runtime
}

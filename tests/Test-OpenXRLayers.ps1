$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/Headset-Resolution.ps1')
function Assert($Value,[string]$Message) { if(!$Value) { throw $Message } }
$fixture=[IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) ('build/openxr-layers-'+[guid]::NewGuid().ToString('N'))))
$created=[Collections.Generic.List[string]]::new()
function Write-Fixture([string]$Name,[string]$Text,[switch]$Bom) {
    $path=Join-Path $fixture $Name
    [IO.File]::WriteAllText($path,$Text,[Text.UTF8Encoding]::new([bool]$Bom)); $created.Add($path); return $path
}
function Manifest([string]$Name,[string]$Library,[string]$Disable) {
    return (@{file_format_version='1.0.0';api_layer=@{name=$Name;library_path=$Library;api_version='1.0';implementation_version='1';description='fixture';disable_environment=$Disable}} | ConvertTo-Json -Depth 3)
}
New-Item -ItemType Directory -Path $fixture | Out-Null
try {
    foreach($valid in 'DISABLE_XR_APILAYER_reshade','DISABLE_OPENXR_x',('DISABLE_XR_'+'A'*180)) { Assert (Test-Grid2VrLayerDisableVariable $valid) "Valid switch rejected: $valid" }
    foreach($invalid in 'PATH','XR_RUNTIME_JSON','DISABLE_XR_','DISABLE_XR_A-B','disable_xr_a','DISABLE_XR_A ',('DISABLE_XR_'+'A'*181),"DISABLE_XR_A`nPATH") {
        Assert !(Test-Grid2VrLayerDisableVariable $invalid) "Unsafe switch accepted: $invalid"
    }

    # Manifests as ReShade and OBS Mirror register them; the BOM matches common installers.
    $reshade=Write-Fixture 'reshade.json' (Manifest 'XR_APILAYER_reshade' '.\ReShade64.dll' 'DISABLE_XR_APILAYER_reshade') -Bom
    $obs=Write-Fixture 'obs-mirror.json' (Manifest 'XR_APILAYER_NOVENDOR_OBSMirror' '.\obs-mirror.dll' 'DISABLE_XR_APILAYER_NOVENDOR_OBSMirror')
    $catalog=@((Read-Grid2VrLayerManifest $reshade),(Read-Grid2VrLayerManifest $obs))
    Assert ($catalog[0].layerName -ceq 'XR_APILAYER_reshade' -and $catalog[0].libraryPath -eq (Join-Path $fixture 'ReShade64.dll') -and
        $catalog[0].disableVariable -ceq 'DISABLE_XR_APILAYER_reshade' -and $catalog[0].manifestHash -match '\A[0-9A-F]{64}\z') 'Manifest identity'
    foreach($bad in @(@('XR_APILAYER_bad','x.dll','PATH'),@('not_a_layer','x.dll','DISABLE_XR_A'),@('XR_APILAYER_empty','','DISABLE_XR_A'),@('XR_APILAYER_none','x.dll',$null))) {
        $path=Write-Fixture 'bad.json' (Manifest $bad[0] $bad[1] $bad[2])
        Assert ($null -eq (Read-Grid2VrLayerManifest $path)) "Unsafe manifest accepted: $($bad[0])"
    }
    Assert ($null -eq (Read-Grid2VrLayerManifest 'relative.json')) 'Relative manifest path'
    Assert ($null -eq (Read-Grid2VrLayerManifest (Write-Fixture 'large.json' ('{"api_layer":{}}'+' '*70000)))) 'Oversized manifest'
    Assert ($null -eq (Read-Grid2VrLayerManifest (Write-Fixture 'broken.json' '{'))) 'Invalid JSON'
    Assert (@(Get-Grid2VrApiLayers) -is [array]) 'Registry inventory'

    # Loader 1.1.53 output with XR_LOADER_DEBUG=warn when every layer fails to load (format captured
    # from the real 32-bit probe with a 64-bit layer DLL).
    $report=@"
GRID2VR OpenXR compatibility query; architecture=x86; session_creation=disabled
enumerate_extensions=0
read_extensions=0
Warning [GENERAL | xrCreateInstance | OpenXR-Loader] : ApiLayerInterface::LoadApiLayers skipping layer XR_APILAYER_reshade, failed to load with message "Failed to open dynamic library $fixture\ReShade64.dll with error 193: 0xc1 (193): %1 is not a valid Win32 application.
"
Warning [GENERAL | xrCreateInstance | OpenXR-Loader] : ApiLayerInterface::LoadApiLayers skipping layer XR_APILAYER_NOVENDOR_OBSMirror, failed to load with message "Failed to open dynamic library $fixture\obs-mirror.dll with error 126: 0x7e (126): The specified module could not be found.
"
Warning [GENERAL | xrCreateInstance | OpenXR-Loader] : ApiLayerInterface::LoadApiLayers skipping layer XR_APILAYER_reshade, failed to load with message "Failed to open dynamic library $fixture\ReShade64.dll with error 193: 0xc1 (193): %1 is not a valid Win32 application.
"
Error [GENERAL | xrCreateInstance | OpenXR-Loader] : Failed loading layer information
Error [GENERAL | xrCreateInstance | OpenXR-Loader] : xrCreateInstance failed
create_instance=-32
"@
    $failures=@(Get-Grid2VrLayerFailures 3 $report)
    Assert ($failures.Count -eq 2 -and $failures[0].layerName -ceq 'XR_APILAYER_reshade' -and $failures[0].windowsError -eq 193 -and $failures[1].windowsError -eq 126) 'Loader failures parsed once each'
    Assert (@(Get-Grid2VrLayerFailures 0 $report).Count -eq 0) 'Successful query has no failures'
    Assert (@(Get-Grid2VrLayerFailures 3 $report.Replace('create_instance=-32','create_instance=-2')).Count -eq 0) 'Only the loader file-access failure qualifies'
    Assert (@(Get-Grid2VrLayerFailures 3 $report.Replace('Failed loading layer information','')).Count -eq 0) 'Loader layer error required'
    Assert (@(Get-Grid2VrLayerFailures 3 $report.Replace("$fixture\obs-mirror.dll",'obs-mirror.dll')).Count -eq 1) 'Search-path libraries are not matched'

    $candidates=@(Resolve-Grid2VrLayerBypasses $failures $catalog)
    Assert ($candidates.Count -eq 2 -and $candidates[1].disableVariable -ceq 'DISABLE_XR_APILAYER_NOVENDOR_OBSMirror') 'Both named layers resolved'
    $moved=@($failures[0],[pscustomobject]@{layerName=$failures[1].layerName;libraryPath='C:\Other\obs-mirror.dll';windowsError=126})
    Assert (@(Resolve-Grid2VrLayerBypasses $moved $catalog).Count -eq 0) 'A different library is not bypassed'
    Assert (@(Resolve-Grid2VrLayerBypasses @([pscustomobject]@{layerName='XR_APILAYER_unknown';libraryPath='C:\x.dll'}) $catalog).Count -eq 0) 'Unregistered layer'
    $shared=Read-Grid2VrLayerManifest (Write-Fixture 'shared.json' (Manifest 'XR_APILAYER_other' 'other.dll' 'DISABLE_XR_APILAYER_reshade'))
    Assert (@(Resolve-Grid2VrLayerBypasses $failures @($catalog+$shared)).Count -eq 0) 'A shared switch would disable an unnamed layer'
    $twin=Read-Grid2VrLayerManifest (Write-Fixture 'twin.json' (Manifest 'XR_APILAYER_reshade' 'twin.dll' 'DISABLE_XR_APILAYER_reshade_twin'))
    Assert (@(Resolve-Grid2VrLayerBypasses $failures @($catalog+$twin)).Count -eq 0) 'Ambiguous layer name'
    Assert (@(Resolve-Grid2VrLayerBypasses @(1..17 | ForEach-Object { $failures[0] }) $catalog).Count -eq 0) 'Failure count bounded'

    # Approvals travel as JSON; only an unchanged registration is applied.
    $transport=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes((ConvertTo-Json @($candidates+$candidates[0]) -Depth 3)))
    $approved=@(ConvertFrom-Grid2VrLayerTransport $transport)
    Assert ($approved.Count -eq 3 -and (Test-Grid2VrLayerIdentity $approved[0] $candidates[0])) 'Transport keeps identity'
    $pascal=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes('[{"LayerName":"XR_APILAYER_reshade","ManifestPath":"'+$reshade.Replace('\','\\')+'","ManifestHash":"'+$candidates[0].manifestHash+'","LibraryPath":"'+$candidates[0].libraryPath.Replace('\','\\')+'","DisableVariable":"DISABLE_XR_APILAYER_reshade"}]'))
    Assert (Test-Grid2VrLayerIdentity @(ConvertFrom-Grid2VrLayerTransport $pascal)[0] $candidates[0]) 'Transport property names'
    $selected=@(Select-Grid2VrLayerBypasses $approved $catalog)
    Assert ($selected.Count -eq 2) 'Duplicate approvals collapse'
    [IO.File]::AppendAllText($obs,' ')
    $changed=@((Read-Grid2VrLayerManifest $reshade),(Read-Grid2VrLayerManifest $obs))
    $selected=@(Select-Grid2VrLayerBypasses $approved $changed)
    Assert ($selected.Count -eq 1 -and $selected[0].layerName -ceq 'XR_APILAYER_reshade') 'A changed manifest needs new approval'
    Assert (@(Select-Grid2VrLayerBypasses @() $catalog).Count -eq 0 -and @(ConvertFrom-Grid2VrLayerTransport '').Count -eq 0) 'No approvals'
    foreach($bad in ('x'*16385),'not-base64!',[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes((ConvertTo-Json @(1..17 | ForEach-Object { $candidates[0] }) -Depth 3)))) {
        $rejected=$false; try { $null=ConvertFrom-Grid2VrLayerTransport $bad } catch { $rejected=$true }; Assert $rejected 'Invalid transport accepted'
    }

    $query=[pscustomobject]@{runtime='VDXR';exitCode=3;output=$report;errors="Error [GENERAL | xrCreateInstance | OpenXR-Loader] : Failed loading layer information`n";disabledLayers=@()}
    Assert ((Get-HeadsetQueryFailure $query) -ceq 'An OpenXR layer could not load: XR_APILAYER_reshade (Windows error 193); XR_APILAYER_NOVENDOR_OBSMirror (Windows error 126).') 'Layer failure message'
    $query.output="create_instance=0`nruntime_properties=0`nget_headset_system=-34`n"; $query.exitCode=4
    Assert ((Get-HeadsetQueryFailure $query) -ceq 'Could not read the headset resolution (query exit 4, get_headset_system=-34). Connect your headset through VDXR and refresh.') 'Failing step named'
    $query.disabledLayers=@('XR_APILAYER_reshade','XR_APILAYER_NOVENDOR_OBSMirror')
    Assert ((Get-HeadsetQueryReport $query) -cmatch '(?m)^disabled_api_layers=XR_APILAYER_reshade,XR_APILAYER_NOVENDOR_OBSMirror\r?$') 'Report names disabled layers'
} finally {
    # Only the files created above; no recursive deletion or linked trees.
    foreach($path in $created | Sort-Object -Unique) { if(Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }
    Remove-Item -LiteralPath $fixture
}
Write-Output 'OpenXR layers: safe switches, manifest identity, loader failures, unambiguous bypasses, revalidated approvals and reports passed.'

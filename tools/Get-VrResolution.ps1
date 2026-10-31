param([ValidateSet('VDXR','SteamVR')][string]$Runtime='VDXR',[ValidateLength(0,16384)][string]$LayerBypassBase64='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Headset-Resolution.ps1')
# Exit 0: headset JSON plus the approved layer bypasses that still apply.
# Exit 3: JSON naming OpenXR layers the user may disable for GRID2VR, then retry.
# Exit 1: any other failure, message on stderr.
try {
    $catalog=@(Get-Grid2VrApiLayers)
    $applied=@(Select-Grid2VrLayerBypasses (ConvertFrom-Grid2VrLayerTransport $LayerBypassBase64) $catalog)
    $query=Invoke-HeadsetQuery (Split-Path $PSScriptRoot -Parent) $Runtime $applied
    if($query.exitCode -eq 0) {
        $headset=ConvertFrom-HeadsetQuery $query.output $Runtime
        $headset.layerBypasses=$applied
        $headset | ConvertTo-Json -Compress -Depth 4
        exit 0
    }
    $failures=@(Get-Grid2VrLayerFailures $query.exitCode (Get-HeadsetQueryText $query))
    $candidates=@(Resolve-Grid2VrLayerBypasses $failures $catalog)
    $message=Get-HeadsetQueryFailure $query
    # A layer that still fails while disabled has no further automatic recovery.
    $repeated=@($candidates | Where-Object { $candidate=$_; @($applied | Where-Object { Test-Grid2VrLayerIdentity $_ $candidate }).Count })
    if($candidates.Count -and !$repeated.Count) {
        [ordered]@{message=$message;layers=$candidates;layerBypasses=$applied;report=(Get-HeadsetQueryReport $query)} | ConvertTo-Json -Compress -Depth 4
        [Console]::Error.WriteLine("$message Launch VR to review it.")
        exit 3
    }
    if($failures.Count) { $message+=' No supported automatic layer bypass is available.' }
    [Console]::Error.WriteLine($message); exit 1
}
catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }

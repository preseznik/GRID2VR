# The native image-size request already supports these sizes. Keep its tested
# 1280x720 default and one world unit per metre; no camera/rendering change.
function Get-HeadsetSettings([int]$Scale=100) {
    if($Scale -lt 25 -or $Scale -gt 200 -or $Scale % 25 -ne 0) { throw 'Headset texture scale must be 25-200% in steps of 25.' }
    $width=[int](1280*$Scale/100)
    $height=[int](720*$Scale/100)
    return [ordered]@{scale=$Scale;width=$width;height=$height;unitsPerMetre=1;request="$width $height 1";basis='1280x720 working profile'}
}
function Write-HeadsetRequest([string]$Path,[int]$Scale=100) {
    $settings=Get-HeadsetSettings $Scale
    $stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {
        $bytes=[Text.Encoding]::ASCII.GetBytes($settings.request)
        $stream.Write($bytes,0,$bytes.Length)
    } finally { $stream.Dispose() }
}

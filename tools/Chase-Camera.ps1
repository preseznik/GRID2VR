# Launch-only preferences. Orbit/zoom are native session state, never persisted here.
function Get-Grid2ChaseConfiguration {
    param(
        [ValidateSet(0,1)][int]$ChaseFreeLook=0,
        [ValidateSet(0,1)][int]$ChaseKeepAngle=0,
        [ValidateSet('left','right','always')][string]$ChaseMouseMode='right',
        [ValidateRange(180,1000)][ValidateScript({ $_ % 10 -eq 0 })][int]$ChaseMaxZoomOut=180,
        [ValidateRange(25,300)][int]$ChaseMouseSensitivity=100,
        [ValidateRange(25,300)][int]$ChaseStickSensitivity=100,
        [ValidateSet(0,1)][int]$ChaseInvertVertical=0
    )
    $mouse=$ChaseMouseMode.ToLowerInvariant()
    $mode=@{left=0;right=1;always=2}[$mouse]
    $line="chase-v1 $ChaseFreeLook $ChaseKeepAngle $mode $ChaseMaxZoomOut $ChaseMouseSensitivity $ChaseStickSensitivity $ChaseInvertVertical`n"
    if($line.Length -gt 64) { throw 'Chase request exceeds its size limit.' }
    return @{
        requested=[ordered]@{freeLook=[bool]$ChaseFreeLook;keepAngle=[bool]$ChaseKeepAngle;mouseMode=$mouse;
            maxZoomOut=$ChaseMaxZoomOut;mouseSensitivity=$ChaseMouseSensitivity;stickSensitivity=$ChaseStickSensitivity;invertVertical=[bool]$ChaseInvertVertical}
        text=$line
    }
}

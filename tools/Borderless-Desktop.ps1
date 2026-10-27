. (Join-Path $PSScriptRoot 'Paths.ps1')
function New-DesktopBorderlessCommand([string]$Root,[string]$Run,[Diagnostics.Process]$GameProcess,[switch]$VrMirror) {
    $info=[Diagnostics.ProcessStartInfo]::new((Get-Grid2Launcher $Root))
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WorkingDirectory=$Root
    $mode=if($VrMirror){'--borderless-vr-mirror'}else{'--borderless-desktop'}
    foreach($arg in @($mode,$Run,[string]$GameProcess.Id,[string]$GameProcess.StartTime.ToUniversalTime().Ticks)) { $info.ArgumentList.Add($arg) }
    return $info
}
function Start-VrMirrorBorderless([string]$Root,[string]$Run,[Diagnostics.Process]$GameProcess) {
    $info=New-DesktopBorderlessCommand $Root $Run $GameProcess -VrMirror
    $helper=[Diagnostics.Process]::Start($info)
    if(!$helper) { throw 'VR mirror borderless helper could not start.' }
    $helper.Dispose()
}
function Start-DesktopBorderless([string]$Root,[string]$Run,[Diagnostics.Process]$GameProcess) {
    $info=New-DesktopBorderlessCommand $Root $Run $GameProcess
    $helper=[Diagnostics.Process]::Start($info)
    if(!$helper) { throw 'Desktop borderless helper could not start.' }
    $helper.Dispose()
}

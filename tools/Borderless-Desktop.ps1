. (Join-Path $PSScriptRoot 'Paths.ps1')
function New-DesktopBorderlessCommand([string]$Root,[string]$Run,[Diagnostics.Process]$GameProcess) {
    $info=[Diagnostics.ProcessStartInfo]::new((Get-Grid2Launcher $Root))
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WorkingDirectory=$Root
    foreach($arg in @('--borderless-desktop',$Run,[string]$GameProcess.Id,[string]$GameProcess.StartTime.ToUniversalTime().Ticks)) { $info.ArgumentList.Add($arg) }
    return $info
}
function Start-DesktopBorderless([string]$Root,[string]$Run,[Diagnostics.Process]$GameProcess) {
    $info=New-DesktopBorderlessCommand $Root $Run $GameProcess
    $helper=[Diagnostics.Process]::Start($info)
    if(!$helper) { throw 'Desktop borderless helper could not start.' }
    $helper.Dispose()
}

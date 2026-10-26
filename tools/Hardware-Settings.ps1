function Assert-HardwareSettingsPath([string]$Path) {
    if(!('GRID2VR.HardwareSettingsPath' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'HardwareSettingsPath.cs') -ErrorAction Stop
    }
    return [GRID2VR.HardwareSettingsPath]::Validate($Path)
}

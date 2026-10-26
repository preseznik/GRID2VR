param([string]$Grid2ExecutablePath, [ValidateSet('Avx','Regular')][string]$Grid2ExecutableVariant='Avx')

function Get-Grid2ExecutableHash([string]$Path,[ValidateSet('Avx','Regular')][string]$Variant='Avx') {
    if(!('GRID2VR.GameExecutable' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'GameExecutable.cs') -ErrorAction Stop
    }
    return [GRID2VR.GameExecutable]::Validate($Path, $Variant -eq 'Avx')
}

if($Grid2ExecutablePath) {
    $ErrorActionPreference='Stop'
    try { Get-Grid2ExecutableHash $Grid2ExecutablePath $Grid2ExecutableVariant }
    catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
}

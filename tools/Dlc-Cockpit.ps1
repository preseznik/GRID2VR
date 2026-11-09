. (Join-Path $PSScriptRoot 'Paths.ps1')

function Get-DlcCockpitRequest([string]$Root,[string]$Game,[ValidateSet('HeadOnly','HideDriver')][string]$Mode,[string]$Car,[bool]$NormalLaunch,[bool]$Desktop=$false,[bool]$ApplySeatPosition=$true,[ValidateRange(-30,30)][int]$FovOffset=0,[bool]$OfflineNormal=$false) {
    $arguments=@((Join-Path $Root 'tools/cockpit_dlc.py'),'--game-root',$Game,'--mode',$(if($Mode -eq 'HeadOnly'){'head-only'}else{'hide-driver'}))
    if($NormalLaunch) { $arguments+='--normal' } else { $arguments+=@('--car',$Car) }
    if($OfflineNormal) { $arguments+='--offline-normal' }
    $seats=Join-Path (Get-Grid2DataRoot $Root) 'seat-positions.json'
    if($ApplySeatPosition -and (Test-Path -LiteralPath $seats)) { $arguments+=@('--alignments',$seats) }
    if($Desktop) { $arguments+=@('--desktop','--fov-offset',[string]$FovOffset) }
    $result=& (Get-Grid2Python $Root) @arguments
    if($LASTEXITCODE) { throw 'DLC cockpit preparation failed; original cameras retained.' }
    return $result | ConvertFrom-Json
}

function Assert-DlcCockpitProxy([string]$Proxy,[bool]$Desktop=$false,[bool]$OfflineNormal=$false) {
    $binary=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Proxy))
    $marker=if($Desktop){'dlc-cockpit-desktop-v1'}else{'dlc-cockpit-v1'}
    if(!$binary.Contains($marker)) { throw 'This native build does not support the requested DLC cockpit cameras. Update GRID2VR before launching.' }
    if($OfflineNormal -and !$binary.Contains('grid2vr-offline-normal-v2')) { throw 'This renderer does not support DLC cockpits in offline Normal launches. Update the GRID2VR renderer before launching.' }
}

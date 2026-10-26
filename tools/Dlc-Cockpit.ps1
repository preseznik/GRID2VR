. (Join-Path $PSScriptRoot 'Paths.ps1')

function Get-DlcCockpitRequest([string]$Root,[string]$Game,[ValidateSet('HeadOnly','HideDriver')][string]$Mode,[string]$Car,[bool]$NormalLaunch) {
    $arguments=@((Join-Path $Root 'tools/cockpit_dlc.py'),'--game-root',$Game,'--mode',$(if($Mode -eq 'HeadOnly'){'head-only'}else{'hide-driver'}))
    if($NormalLaunch) { $arguments+='--normal' } else { $arguments+=@('--car',$Car) }
    $seats=Join-Path (Get-Grid2DataRoot $Root) 'seat-positions.json'
    if(Test-Path -LiteralPath $seats) { $arguments+=@('--alignments',$seats) }
    $result=& (Get-Grid2Python $Root) @arguments
    if($LASTEXITCODE) { throw 'DLC cockpit preparation failed; original cameras retained.' }
    return $result | ConvertFrom-Json
}

function Assert-DlcCockpitProxy([string]$Proxy) {
    $binary=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Proxy))
    if(!$binary.Contains('dlc-cockpit-v1')) { throw 'This native build does not support DLC cockpit cameras. Update GRID2VR before launching.' }
}

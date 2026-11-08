. (Join-Path $PSScriptRoot 'Paths.ps1')

function New-SeatSessionRequest([string]$Root,[string]$Run,[bool]$Desktop,[bool]$Positions,[int]$Fov,[bool]$HideDriver) {
    $arguments=@((Join-Path $Root 'tools/seat_session.py'),'--prepare','--settings',(Join-Path (Get-Grid2DataRoot $Root) 'seat-positions.json'),
        '--request',(Join-Path $Run 'seat-menu.request'),'--desktop',[string][int]$Desktop,'--positions',[string][int]$Positions,
        '--fov',[string]$Fov,'--hide-driver',[string][int]$HideDriver)
    $result=& (Get-Grid2Python $Root) @arguments
    if($LASTEXITCODE) { throw 'In-game seat tool preparation failed; no game files changed.' }
    return $result | ConvertFrom-Json
}

function Save-SeatSessionCommit([string]$Root,[string]$Run) {
    $request=Join-Path $Run 'seat-menu.request';$commit=Join-Path $Run 'seat-menu.commit'
    if(!(Test-Path -LiteralPath $commit)) { return }
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'In-game seat commit retained while GRID 2 is running.' }
    $arguments=@((Join-Path $Root 'tools/seat_session.py'),'--settings',(Join-Path (Get-Grid2DataRoot $Root) 'seat-positions.json'),
        '--request',$request,'--commit',$commit)
    $result=& (Get-Grid2Python $Root) @arguments
    if($LASTEXITCODE) { throw "In-game seat settings could not be saved; commit retained in: $Run" }
    $null=$result | ConvertFrom-Json
    [IO.File]::WriteAllText((Join-Path $Run 'seat-commit-applied.json'),[string]$result)
}

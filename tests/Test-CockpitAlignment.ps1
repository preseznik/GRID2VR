$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Cockpit-Assets.ps1')
$fixture=Join-Path $root ('build/alignment-test-'+[guid]::NewGuid().ToString('N'))
function Require([bool]$value,[string]$reason) { if(!$value) { throw $reason } }
function Reject([scriptblock]$action) { $failed=$false;try{& $action | Out-Null}catch{$failed=$true};Require $failed 'Invalid alignment was accepted.' }
function Get-Process { } # Only fixture files may be installed/restored here.
function Hash([string]$path) { (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() }
try {
    foreach($dir in @('tools/data','build/win32','game/cars/models/s20','game/cars/models/pag','game/cutscene','run','artifacts/cockpit-cameras/s20','artifacts/cockpit-cameras/pag')) {
        [IO.Directory]::CreateDirectory((Join-Path $fixture $dir)) | Out-Null
    }
    foreach($name in @('cockpit_alignment.py','session_config.py','game_executable.py')) { Copy-Item -LiteralPath (Join-Path $root "tools/$name") -Destination (Join-Path $fixture "tools/$name") }
    Copy-Item -LiteralPath (Join-Path $root 'build/win32/CMakeCache.txt') -Destination (Join-Path $fixture 'build/win32/CMakeCache.txt')
    $catalog=Read-CockpitAssetCatalog $root
    $catalog.cars=@($catalog.cars | Where-Object car -In @('s20','pag'))
    $game=Join-Path $fixture 'game';$run=Join-Path $fixture 'run'
    [IO.File]::WriteAllText((Join-Path $game 'grid2_avx.exe'),'fixture executable')
    $script:CockpitCameraExecutableHash=Hash (Join-Path $game 'grid2_avx.exe')
# Fixture-only executable validator; preserves the transaction's changed-file checks.
function Get-Grid2ExecutableHash([string]$Path,[string]$Variant='Avx') {
    $actual=(Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash.ToLowerInvariant()
    if($actual -ne $script:CockpitCameraExecutableHash) { throw 'Changed fixture executable.' }
    return $actual
}

    foreach($car in $catalog.cars) {
        $installed=Join-Path $game "cars/models/$($car.car)/cameras.xml"
        [IO.File]::WriteAllText($installed,"original $($car.car)");$car.originalSha256=Hash $installed
        foreach($mode in @('HeadOnly','HideDriver')) { Copy-Item -LiteralPath (Join-Path $root "artifacts/cockpit-cameras/$($car.car)/$mode.bin") -Destination (Join-Path $fixture "artifacts/cockpit-cameras/$($car.car)/$mode.bin") }
    }
    [IO.File]::WriteAllText((Join-Path $game $catalog.intro.name),'original intro');$catalog.intro.originalSha256=Hash (Join-Path $game $catalog.intro.name)
    Copy-Item -LiteralPath (Join-Path $root 'artifacts/cockpit-cameras/intro.xml') -Destination (Join-Path $fixture 'artifacts/cockpit-cameras/intro.xml')
    $path=Join-Path $fixture 'tools/data/cockpit-assets.json';$catalog | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path
    $script:CockpitAssetsRoot=$fixture;$script:CockpitAssetsCatalogHash=Hash $path
    $seats=Join-Path $fixture 'artifacts/seat-positions.json'
    @{Version=1;Cars=@{s20=@{Height=-12;Forward=5;Right=3};pag=@{Height=-5;Forward=0;Right=0}}} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $seats
    $entries=@(Get-CockpitAssetSelection $fixture $game HeadOnly s20 $true)
    Require ($entries.Count -eq 3) 'Normal alignment lost cars or startup handoff.'
    foreach($entry in $entries | Where-Object car -NE intro) { Require ($entry.alignment.Height -lt 0) 'Missing per-car offset.' }
    $null=New-CockpitAssetsDeployment $game $run $fixture $entries
    Install-CockpitAssets $game $run $entries
    # Recovery must use its immutable snapshot, even if the user subsequently changes preferences.
    [IO.File]::WriteAllText($seats,'{"Version":1,"Cars":{"s20":{"Height":35,"Forward":0,"Right":0}}}')
    $reloaded=$entries | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    Restore-CockpitAssets $game $run $reloaded
    foreach($entry in $reloaded) { Assert-CockpitCameraHash (Join-Path $game $entry.name) $entry.originalSha256 }
    $bad=$entries[0].Clone();$bad.alignment=@{Height=1;Forward=0;Right=0}
    Reject { Assert-CockpitAssetEntries @($bad) }
    foreach($value in @('1.5','51','true','null')) {
        [IO.File]::WriteAllText($seats,('{"Version":1,"Cars":{"s20":{"Height":'+$value+',"Forward":0,"Right":0}}}'))
        Reject { Read-SeatAlignments $fixture }
    }
    # FOV-only Desktop does not consume VR seat offsets or force the startup view.
    @{Version=1;Cars=@{s20=@{Height=12;Forward=4;Right=2}}} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $seats
    $desktopRun=Join-Path $fixture 'desktop';New-Item -ItemType Directory -Path $desktopRun | Out-Null
    $desktop=@(Get-CockpitAssetSelection $fixture $game HeadOnly s20 $false -ApplySeatPosition:$false -FovOffset:10 -IncludeIntro:$false)
    Require ($desktop.Count -eq 1 -and !$desktop[0].alignment -and $desktop[0].fovOffset -eq 10) 'Desktop FOV unexpectedly included VR seat offsets.'
    $null=New-CockpitAssetsDeployment $game $desktopRun $fixture $desktop
    Install-CockpitAssets $game $desktopRun $desktop
    Assert-CockpitCameraHash (Join-Path $game $desktop[0].name) $desktop[0].sha256
    [IO.File]::WriteAllText($seats,'{"Version":1,"DesktopFov":-10,"Cars":{}}')
    $snapshot=$desktop | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    Restore-CockpitAssets $game $desktopRun @($snapshot)
    Assert-CockpitCameraHash (Join-Path $game $desktop[0].name) $desktop[0].originalSha256
    $normalDesktop=@(Get-CockpitAssetSelection $fixture $game HeadOnly s20 $true -ApplySeatPosition:$false -FovOffset:-10 -IncludeIntro:$false)
    Require ($normalDesktop.Count -eq 2 -and @($normalDesktop | Where-Object car -EQ intro).Count -eq 0) 'Desktop changed the first-career startup view.'
    foreach($value in @(31,-31,'10',$true)) {
        $bad=$desktop[0].Clone();$bad.fovOffset=$value
        Reject { Assert-CockpitAssetEntries @($bad) }
    }
    $bad=$desktop[0].Clone();$bad.fovOffset=-10
    Reject { Assert-CockpitAssetEntries @($bad) }
    Write-Output 'Seat/FOV deployment, Desktop/VR separation, exact recovery after settings changes and tampering guards passed.'
} finally {
    $full=[IO.Path]::GetFullPath($fixture)
    if($full.StartsWith((Join-Path $root 'build/alignment-test-'),[StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}

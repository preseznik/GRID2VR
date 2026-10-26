$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Cockpit-Assets.ps1')
$fixture=Join-Path $root ('build/cockpit-assets-test-'+[guid]::NewGuid().ToString('N'))
$game=Join-Path $fixture 'game'
[IO.Directory]::CreateDirectory((Join-Path $fixture 'tools/data')) | Out-Null
[IO.Directory]::CreateDirectory($game) | Out-Null
$script:CockpitAssetsRoot=$fixture
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() }
function Require([bool]$Condition,[string]$Message) { if(!$Condition) { throw $Message } }
function Reject([scriptblock]$Action) { $failed=$false;try { & $Action | Out-Null }catch {$failed=$true};Require $failed 'Unsafe cockpit asset operation accepted.' }
$script:running=$false
function Get-Process { if($script:running) { [pscustomobject]@{Id=42} } }
try {
    [IO.File]::WriteAllText((Join-Path $game 'grid2_avx.exe'),'supported executable fixture')
    $script:CockpitCameraExecutableHash=Hash (Join-Path $game 'grid2_avx.exe')
# Fixture-only executable validator; preserves the transaction's changed-file checks.
function Get-Grid2ExecutableHash([string]$Path,[string]$Variant='Avx') {
    $actual=(Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash.ToLowerInvariant()
    if($actual -ne $script:CockpitCameraExecutableHash) { throw 'Changed fixture executable.' }
    return $actual
}

    $cars=@()
    foreach($car in @('s20','pag')) {
        $installed=Join-Path $game "cars/models/$car/cameras.xml"
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($installed)) | Out-Null
        [IO.File]::WriteAllText($installed,"original $car")
        $row=@{car=$car;originalSha256=(Hash $installed)}
        foreach($mode in @('HeadOnly','HideDriver')) {
            $asset=Join-Path $fixture "artifacts/cockpit-cameras/$car/$mode.bin"
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($asset)) | Out-Null
            [IO.File]::WriteAllText($asset,"$car $mode")
            $row[$mode]=Hash $asset
        }
        $cars+=$row
    }
    $intro=Join-Path $game 'cutscene/intro_buildup.xml'
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($intro)) | Out-Null
    [IO.File]::WriteAllText($intro,'original intro')
    $introAsset=Join-Path $fixture 'artifacts/cockpit-cameras/intro.xml'
    [IO.File]::WriteAllText($introAsset,'cockpit first-career handoff')
    $start=Join-Path $game 'cutscene/startsequence.xml'
    [IO.File]::WriteAllText($start,'original start sequence')
    $startAsset=Join-Path $fixture 'artifacts/cockpit-start/startsequence.xml'
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($startAsset)) | Out-Null
    [IO.File]::WriteAllText($startAsset,'cockpit race-start handoff')
    $catalogPath=Join-Path $fixture 'tools/data/cockpit-assets.json'
    @{schema=1;cars=$cars;startView='bonnet';intro=@{name='cutscene/intro_buildup.xml';originalSha256=(Hash $intro);sha256=(Hash $introAsset)};
        startsequence=@{name='cutscene/startsequence.xml';originalSha256=(Hash $start);sha256=(Hash $startAsset)}} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $catalogPath
    $script:CockpitAssetsCatalogHash=Hash $catalogPath
    Require (@(Get-CockpitAssetSelection $fixture $game HeadOnly 's20' $false).Count -eq 1) 'Camera-only diagnostics must retain their original startup sequence.'
    $selected=@(Get-CockpitAssetSelection $fixture $game HeadOnly 's20' $false $true)
    Require ($selected.Count -eq 2 -and $selected[0].car -ceq 's20' -and $selected[1].car -ceq 'startsequence') 'Selected launch must include only its car and race-start handoff.'
    Require (@(Get-CockpitAssetSelection $fixture $game HeadOnly 's20' $true).Count -eq 3) 'Normal does not cover menu-selected cars and first-career handoff.'
    $dlc=@(Get-CockpitAssetSelection $fixture $game HeadOnly 'ind' $false $true $true)
    Require ($dlc.Count -eq 1 -and $dlc[0].car -ceq 'startsequence') 'Native DLC cockpit must deploy only its shared startup asset.'
    Reject {Get-CockpitAssetSelection $fixture $game HeadOnly '../bad' $false}
    foreach($mode in @('HeadOnly','HideDriver')) {
      foreach($normal in @($true,$false)) {
        foreach($phase in @('prepared','partial','installed','temporary')) {
            $run=Join-Path $fixture "$mode-$normal-$phase"; [IO.Directory]::CreateDirectory($run) | Out-Null
            $entries=@(Get-CockpitAssetSelection $fixture $game $mode 's20' $normal $true)
            $null=New-CockpitAssetsDeployment $game $run $fixture $entries
            if($phase -eq 'installed') { Install-CockpitAssets $game $run $entries }
            if($phase -eq 'partial') {
                $paths=Get-CockpitAssetPaths $game $run $entries[0]
                Write-CockpitCameraAtomic $paths $paths.source $entries[0].originalSha256 $entries[0].sha256
            }
            if($phase -eq 'temporary') {
                $paths=Get-CockpitAssetPaths $game $run $entries[0]
                [IO.File]::Copy($paths.source,$paths.temporary,$false)
            }
            Restore-CockpitAssets $game $run $entries
            Restore-CockpitAssets $game $run $entries
            foreach($entry in $entries) { Assert-CockpitCameraHash (Join-Path $game $entry.name) $entry.originalSha256 }
        }
      }
    }
    $run=Join-Path $fixture 'guards';[IO.Directory]::CreateDirectory($run) | Out-Null
    $entries=@(Get-CockpitAssetSelection $fixture $game HeadOnly '' $true)
    $null=New-CockpitAssetsDeployment $game $run $fixture $entries
    $paths=Get-CockpitAssetPaths $game $run $entries[1]
    [IO.File]::WriteAllText($paths.source,'foreign prepared source')
    Reject {Install-CockpitAssets $game $run $entries}
    # A later entry failure must be caught before changing the first car.
    Assert-CockpitCameraHash (Join-Path $game $entries[0].name) $entries[0].originalSha256
    [IO.File]::Copy((Join-Path $fixture 'artifacts/cockpit-cameras/pag/HeadOnly.bin'),$paths.source,$true)
    Install-CockpitAssets $game $run $entries
    [IO.File]::WriteAllText($paths.target,'foreign installed car')
    Reject {Restore-CockpitAssets $game $run $entries}
    Assert-CockpitCameraHash (Join-Path $game $entries[0].name) $entries[0].sha256
    [IO.File]::Copy($paths.source,$paths.target,$true)
    $script:running=$true
    Reject {Restore-CockpitAssets $game $run $entries}
    $script:running=$false
    foreach($index in @(0,2)) {
        foreach($field in @('name','car','mode','sha256','originalSha256')) {
            $bad=$entries[$index].Clone();$bad[$field]='../foreign'
            Reject {Restore-CockpitAssets $game $run @($bad)}
        }
    }
    Reject {Restore-CockpitAssets $game $run @($entries[0],$entries[0])}
    Restore-CockpitAssets $game $run $entries
    # The startup entry receives the same foreign-file and journal guards as cameras.
    foreach($field in @('name','car','mode','sha256','originalSha256')) {
        $bad=$selected[1].Clone();$bad[$field]='../foreign'
        Reject {Assert-CockpitAssetEntries @($bad)}
    }
    [IO.File]::AppendAllText($startAsset,' foreign')
    Reject {Get-CockpitAssetSelection $fixture $game HeadOnly 's20' $false $true}
    [IO.File]::AppendAllText($catalogPath,' ')
    Reject {Get-CockpitAssetSelection $fixture $game HeadOnly '' $true}
    Write-Output 'Per-car cockpit selection, Normal coverage, both visibility modes, interrupted multi-car recovery and foreign/process/catalog guards passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($resolved) -ne [IO.Path]::GetFullPath((Join-Path $root 'build')) -or
       [IO.Path]::GetFileName($resolved) -notmatch '^cockpit-assets-test-[0-9a-f]{32}$') { throw 'Fixture cleanup boundary invalid.' }
    if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) { throw 'Linked fixture retained.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

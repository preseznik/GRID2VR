. (Join-Path $PSScriptRoot 'Paths.ps1')
# Per-car camera assets only; stereo rendering and saved camera preferences are untouched.
. (Join-Path $PSScriptRoot 'Cockpit-Camera.ps1')
$script:CockpitAssetsRoot=Split-Path $PSScriptRoot -Parent
$script:CockpitAssetsCatalogHash='2e4185a36dd6c1878449d4ab2b6bce596156271483a650b75f749cd2b120645c'

function Read-CockpitAssetCatalog([string]$Root) {
    $path=Join-Path $Root 'tools/data/cockpit-assets.json'
    Assert-CockpitCameraHash $path $script:CockpitAssetsCatalogHash
    if((Get-Item -LiteralPath $path).Length -gt 256KB) { throw 'Cockpit catalog exceeds its bound.' }
    $catalog=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if($catalog.schema -ne 1 -or !$catalog.cars -or @($catalog.cars).Count -gt 64) { throw 'Invalid cockpit asset catalog.' }
    $seen=@{}
    foreach($car in $catalog.cars) {
        if($car.car -cnotmatch '^[a-z0-9]{3}$' -or $seen.ContainsKey($car.car) -or
           $car.originalSha256 -cnotmatch '^[0-9a-f]{64}$' -or
           $car.HeadOnly -cnotmatch '^[0-9a-f]{64}$' -or $car.HideDriver -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid cockpit car record.' }
        $seen[$car.car]=$true
    }
    if($catalog.startView -cne 'bonnet' -or $catalog.intro.name -cne 'cutscene/intro_buildup.xml' -or
       $catalog.intro.originalSha256 -cnotmatch '^[0-9a-f]{64}$' -or $catalog.intro.sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid cockpit startup record.' }
    return $catalog
}

function Get-CockpitAssetSource([string]$Root,$Entry) {
    if($Entry.car -ceq 'intro') { return Join-Path (Get-Grid2DataRoot $Root) 'cockpit-cameras/intro.xml' }
    return Join-Path (Get-Grid2DataRoot $Root) "cockpit-cameras/$($Entry.car)/$($Entry.mode).bin"
}

function Assert-SeatOffset($Offset) {
    if(!$Offset) { throw 'Missing cockpit alignment.' }
    foreach($axis in @('Height','Forward','Right')) {
        $value=$Offset.$axis
        if(($value -isnot [int] -and $value -isnot [long]) -or $value -lt -50 -or $value -gt 50) { throw 'Cockpit adjustments must be whole centimetres within -50..50.' }
    }
}

function Read-SeatAlignments([string]$Root) {
    $path=Join-Path (Get-Grid2DataRoot $Root) 'seat-positions.json'
    if(!(Test-Path -LiteralPath $path)) { return @{} }
    Assert-CockpitCameraPlainPath $path
    if((Get-Item -LiteralPath $path).Length -gt 32KB) { throw 'Seat settings exceed their bound.' }
    $value=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
    if($value.Version -ne 1 -or $value.Cars -isnot [Collections.IDictionary] -or $value.Cars.Count -gt 64) { throw 'Invalid seat settings.' }
    foreach($car in $value.Cars.Keys) {
        if($car -cnotmatch '^[a-z0-9]{3}$') { throw 'Invalid seat car.' }
        Assert-SeatOffset $value.Cars[$car]
    }
    return $value.Cars
}

function Invoke-CockpitAlignment([string]$Root,$Entry,[string]$BaseHash,[string]$Output='') {
    Assert-SeatOffset $Entry.alignment
    $python=Get-Grid2Python $Root
    $args=@((Join-Path $Root 'tools/cockpit_alignment.py'),'--source',(Get-CockpitAssetSource $Root $Entry),'--sha256',$BaseHash,
        '--car',$Entry.car,'--height',[string]$Entry.alignment.Height,'--forward',[string]$Entry.alignment.Forward,'--right',[string]$Entry.alignment.Right)
    if($Output) { $args+=@('--output',$Output) }
    $result=& $python @args
    if($LASTEXITCODE) { throw 'Cockpit alignment validation failed.' }
    return ($result | ConvertFrom-Json).sha256
}

function Get-CockpitAssetSelection([string]$Root,[string]$Game,[ValidateSet('HeadOnly','HideDriver')][string]$Mode,[string]$Car,[bool]$NormalLaunch) {
    $null=Get-Grid2ExecutableHash (Join-Path $Game 'grid2_avx.exe')
    $catalog=Read-CockpitAssetCatalog $Root
    $alignments=Read-SeatAlignments $Root
    $cars=@($catalog.cars | Where-Object {$NormalLaunch -or $_.car -ceq $Car})
    if(!$cars.Count) { throw "No cockpit calibration is available for car '$Car'. Disable Cockpit view to use its original cameras." }
    $result=@()
    foreach($item in $cars) {
        $name="cars/models/$($item.car)/cameras.xml"
        Assert-CockpitCameraHash (Join-Path $Game $name) $item.originalSha256
        $asset=Join-Path (Get-Grid2DataRoot $Root) "cockpit-cameras/$($item.car)/$Mode.bin"
        Assert-CockpitCameraHash $asset $item.$Mode
        if((Get-Item -LiteralPath $asset).Length -gt 512KB) { throw 'Cockpit asset exceeds its bound.' }
        $entry=@{car=$item.car;name=$name;mode=$Mode;originalSha256=$item.originalSha256;sha256=$item.$Mode}
        if($alignments.Contains($item.car)) {
            $offset=$alignments[$item.car]
            if($offset.Height -or $offset.Forward -or $offset.Right) {
                $entry.alignment=$offset
                $entry.sha256=Invoke-CockpitAlignment $Root $entry $item.$Mode
            }
        }
        $result+=$entry
    }
    if($NormalLaunch) {
        $intro=$catalog.intro
        Assert-CockpitCameraHash (Join-Path $Game $intro.name) $intro.originalSha256
        $entry=@{car='intro';name=$intro.name;mode='Intro';originalSha256=$intro.originalSha256;sha256=$intro.sha256}
        Assert-CockpitCameraHash (Get-CockpitAssetSource $Root $entry) $intro.sha256
        $result+=$entry
    }
    return $result
}

function Assert-CockpitAssetEntries($Entries) {
    $items=@($Entries)
    if(!$items.Count -or $items.Count -gt 65) { throw 'Invalid cockpit deployment size.' }
    $catalog=Read-CockpitAssetCatalog $script:CockpitAssetsRoot
    $seen=@{}
    foreach($entry in $items) {
        if($entry.car -ceq 'intro') {
            if($seen.ContainsKey('intro') -or $entry.name -cne $catalog.intro.name -or $entry.mode -cne 'Intro' -or
               $entry.originalSha256 -ne $catalog.intro.originalSha256 -or $entry.sha256 -ne $catalog.intro.sha256) { throw 'Invalid first-career cockpit recovery entry.' }
            $seen['intro']=$true
            continue
        }
        if(!$entry -or $entry.car -cnotmatch '^[a-z0-9]{3}$' -or $seen.ContainsKey($entry.car) -or
           $entry.name -cne "cars/models/$($entry.car)/cameras.xml" -or $entry.mode -cnotin @('HeadOnly','HideDriver')) { throw 'Invalid cockpit recovery entry.' }
        $car=@($catalog.cars | Where-Object car -CEQ $entry.car)
        if($car.Count -ne 1 -or $entry.originalSha256 -ne $car[0].originalSha256) { throw 'Unrecognized cockpit recovery hashes.' }
        $expected=$car[0].($entry.mode)
        # Recompute from the pinned base and the journal snapshot, never today's settings.
        if($entry.alignment) { $expected=Invoke-CockpitAlignment $script:CockpitAssetsRoot $entry $expected }
        if($entry.sha256 -ne $expected) { throw 'Unrecognized aligned cockpit hash.' }
        $seen[$entry.car]=$true
    }
}

function Get-CockpitAssetPaths([string]$Game,[string]$Run,$Entry) {
    # Reuse the installation/backup containment and reparse-point guards.
    $null=Get-CockpitCameraPaths $Game $Run
    $intro=$Entry.car -ceq 'intro' -and $Entry.name -ceq 'cutscene/intro_buildup.xml'
    if(!$intro -and ($Entry.car -cnotmatch '^[a-z0-9]{3}$' -or $Entry.name -cne "cars/models/$($Entry.car)/cameras.xml")) { throw 'Invalid cockpit path.' }
    $paths=@{
        target=(Join-Path $Game $Entry.name)
        temporary=$(if($intro){Join-Path $Game 'cutscene/GRID2VR.cockpit.tmp'}else{Join-Path $Game "cars/models/$($Entry.car)/GRID2VR.cockpit.tmp"})
        source=(Join-Path $Run "cockpit-assets/$($Entry.car)-applied.xml")
        backup=(Join-Path $Run "cockpit-assets/$($Entry.car)-original.xml")
    }
    foreach($path in $paths.Values) { Assert-CockpitCameraPlainPath $path }
    return $paths
}

function New-CockpitAssetsDeployment([string]$Game,[string]$Run,[string]$Root,$Entries) {
    Assert-CockpitAssetEntries $Entries
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before preparing cockpit assets.' }
    foreach($entry in $Entries) {
        $paths=Get-CockpitAssetPaths $Game $Run $entry
        Assert-CockpitCameraHash $paths.target $entry.originalSha256
        if(!$entry.alignment) { Assert-CockpitCameraHash (Get-CockpitAssetSource $Root $entry) $entry.sha256 }
        foreach($path in @($paths.source,$paths.backup,$paths.temporary)) { if(Test-Path -LiteralPath $path) { throw 'Existing cockpit recovery data retained.' } }
    }
    [IO.Directory]::CreateDirectory((Join-Path $Run 'cockpit-assets')) | Out-Null
    foreach($entry in $Entries) {
        $paths=Get-CockpitAssetPaths $Game $Run $entry
        [IO.File]::Copy($paths.target,$paths.backup,$false)
        if($entry.alignment) {
            $catalog=Read-CockpitAssetCatalog $Root
            $base=@($catalog.cars | Where-Object car -CEQ $entry.car)[0].($entry.mode)
            $null=Invoke-CockpitAlignment $Root $entry $base $paths.source
        } else { [IO.File]::Copy((Get-CockpitAssetSource $Root $entry),$paths.source,$false) }
        Assert-CockpitCameraHash $paths.backup $entry.originalSha256
        Assert-CockpitCameraHash $paths.source $entry.sha256
    }
    return @($Entries)
}

function Install-CockpitAssets([string]$Game,[string]$Run,$Entries) {
    Assert-CockpitAssetEntries $Entries
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing GRID 2 process retained.' }
    $null=Get-Grid2ExecutableHash (Join-Path $Game 'grid2_avx.exe')
    # Validate the entire set before the first installed write.
    foreach($entry in $Entries) {
        $paths=Get-CockpitAssetPaths $Game $Run $entry
        Assert-CockpitCameraHash $paths.target $entry.originalSha256
        Assert-CockpitCameraHash $paths.backup $entry.originalSha256
        Assert-CockpitCameraHash $paths.source $entry.sha256
        if(Test-Path -LiteralPath $paths.temporary) { throw 'Existing cockpit temporary file retained.' }
    }
    foreach($entry in $Entries) {
        $paths=Get-CockpitAssetPaths $Game $Run $entry
        Write-CockpitCameraAtomic $paths $paths.source $entry.originalSha256 $entry.sha256
    }
}

function Restore-CockpitAssets([string]$Game,[string]$Run,$Entries) {
    Assert-CockpitAssetEntries $Entries
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'GRID 2 must exit before restoring cockpit assets.' }
    foreach($entry in $Entries) {
        $paths=Get-CockpitAssetPaths $Game $Run $entry
        Assert-CockpitCameraHash $paths.backup $entry.originalSha256
        if(!(Test-Path -LiteralPath $paths.target -PathType Leaf) -or
           (Get-FileHash -LiteralPath $paths.target).Hash -notin @($entry.originalSha256,$entry.sha256)) { throw 'Foreign or missing cockpit camera retained.' }
        if((Test-Path -LiteralPath $paths.temporary) -and
           (Get-FileHash -LiteralPath $paths.temporary).Hash -notin @($entry.originalSha256,$entry.sha256)) { throw 'Foreign cockpit temporary file retained.' }
    }
    foreach($entry in $Entries) {
        $paths=Get-CockpitAssetPaths $Game $Run $entry
        if(Test-Path -LiteralPath $paths.temporary) { Remove-Item -LiteralPath $paths.temporary }
        if((Get-FileHash -LiteralPath $paths.target).Hash -eq $entry.sha256) { Write-CockpitCameraAtomic $paths $paths.backup $entry.sha256 $entry.originalSha256 }
        Assert-CockpitCameraHash $paths.target $entry.originalSha256
    }
}

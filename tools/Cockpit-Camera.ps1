# Opt-in S2000 camera asset only. The native stereo renderer is unchanged.
# Applied hashes identify the two offline round-tripped, fixed-position presets.
$script:CockpitCameraOriginalHash='cc6883b9277ca1053e6ac8653f03c8ece95d2ddd5b871a448ef2665447ea24df'
$script:CockpitCameraExecutableHash='519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
$script:CockpitCameraHashes=@{HeadOnly='367d0c7ab431455f1dded395dd311aee18600844e272d8d9b51fd597a2645b97';HideDriver='89872e642926e462481a1c7cfb50d75358893d11696ed7638de1e55842514584'}
$script:CockpitCameraFolders=@{HeadOnly='head-only';HideDriver='hide-driver'}

function Assert-CockpitCameraPlainPath([string]$Path) {
    if(![IO.Path]::IsPathFullyQualified($Path)) { throw 'Cockpit camera paths must be absolute.' }
    for($part=[IO.Path]::GetFullPath($Path);$part;$part=[IO.Path]::GetDirectoryName($part)) {
        if((Test-Path -LiteralPath $part) -and ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Linked cockpit camera path retained: $part"
        }
    }
}

function Assert-CockpitCameraHash([string]$Path,[string]$Expected) {
    Assert-CockpitCameraPlainPath $Path
    if($Expected -notmatch '^[0-9a-f]{64}$' -or !(Test-Path -LiteralPath $Path -PathType Leaf) -or
       (Get-FileHash -LiteralPath $Path).Hash -ne $Expected) { throw "Changed or missing cockpit camera input retained: $Path" }
}

function Get-CockpitCameraPaths([string]$Game,[string]$Run) {
    Assert-CockpitCameraPlainPath $Game; Assert-CockpitCameraPlainPath $Run
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    $runPath=[IO.Path]::GetFullPath($Run).TrimEnd('\')
    if($runPath -eq $gamePath -or $runPath.StartsWith($gamePath+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Cockpit backup must be outside the installation.' }
    if(!(Test-Path -LiteralPath $gamePath -PathType Container) -or !(Test-Path -LiteralPath $runPath -PathType Container)) { throw 'Cockpit game/run directory is missing.' }
    $paths=@{
        target=(Join-Path $gamePath 'cars/models/s20/cameras.xml')
        temporary=(Join-Path $gamePath 'cars/models/s20/GRID2VR.cockpit.tmp')
        source=(Join-Path $runPath 'cockpit-camera.xml')
        backup=(Join-Path $runPath 'cockpit-camera-original.xml')
        executable=(Join-Path $gamePath 'grid2_avx.exe')
    }
    foreach($path in $paths.Values) { Assert-CockpitCameraPlainPath $path }
    return $paths
}

function Get-CockpitCameraCandidate([string]$Root,[ValidateSet('HeadOnly','HideDriver')][string]$Mode) {
    $path=Join-Path $Root ('build/cockpit-candidate-20261025/'+$script:CockpitCameraFolders[$Mode]+'/cameras.xml')
    Assert-CockpitCameraHash $path $script:CockpitCameraHashes[$Mode]
    if((Get-Item -LiteralPath $path).Length -gt 512KB) { throw 'Cockpit camera candidate exceeds the bound.' }
    return $path
}

function Assert-CockpitCameraOriginal([string]$Game) {
    Assert-CockpitCameraHash (Join-Path $Game 'grid2_avx.exe') $script:CockpitCameraExecutableHash
    Assert-CockpitCameraHash (Join-Path $Game 'cars/models/s20/cameras.xml') $script:CockpitCameraOriginalHash
}

function Assert-CockpitCameraEntry($Entry) {
    if(!$Entry -or $Entry.name -cne 'cars/models/s20/cameras.xml' -or $Entry.car -cne 's20' -or
       $Entry.mode -cnotin @('HeadOnly','HideDriver') -or $Entry.originalSha256 -ne $script:CockpitCameraOriginalHash -or
       $Entry.sha256 -ne $script:CockpitCameraHashes[$Entry.mode] -or $Entry.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'Unsupported cockpit camera recovery entry.'
    }
}

function New-CockpitCameraDeployment([string]$Game,[string]$Run,[ValidateSet('HeadOnly','HideDriver')][string]$Mode,[string]$Root) {
    $paths=Get-CockpitCameraPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before preparing the cockpit camera.' }
    Assert-CockpitCameraOriginal $Game
    $candidate=Get-CockpitCameraCandidate $Root $Mode
    foreach($path in @($paths.source,$paths.backup,$paths.temporary)) {
        if(Test-Path -LiteralPath $path) { throw "Existing cockpit recovery file retained: $path" }
    }
    [IO.File]::Copy($paths.target,$paths.backup,$false)
    Assert-CockpitCameraHash $paths.backup $script:CockpitCameraOriginalHash
    [IO.File]::Copy($candidate,$paths.source,$false)
    Assert-CockpitCameraHash $paths.source $script:CockpitCameraHashes[$Mode]
    return @{name='cars/models/s20/cameras.xml';car='s20';mode=$Mode;originalSha256=$script:CockpitCameraOriginalHash;sha256=$script:CockpitCameraHashes[$Mode]}
}

function Write-CockpitCameraAtomic($Paths,[string]$Source,[string]$Before,[string]$After) {
    [IO.File]::Copy($Source,$Paths.temporary,$false)
    Assert-CockpitCameraHash $Paths.temporary $After
    $file=[IO.File]::Open($Paths.temporary,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $file.Flush($true) } finally { $file.Dispose() }
    Assert-CockpitCameraHash $Paths.target $Before
    [IO.File]::Replace($Paths.temporary,$Paths.target,[Management.Automation.Language.NullString]::Value)
    Assert-CockpitCameraHash $Paths.target $After
}

function Install-CockpitCamera([string]$Game,[string]$Run,$Entry) {
    Assert-CockpitCameraEntry $Entry
    $paths=Get-CockpitCameraPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing GRID 2 process retained.' }
    Assert-CockpitCameraHash $paths.executable $script:CockpitCameraExecutableHash
    Assert-CockpitCameraHash $paths.target $script:CockpitCameraOriginalHash
    Assert-CockpitCameraHash $paths.backup $script:CockpitCameraOriginalHash
    Assert-CockpitCameraHash $paths.source $Entry.sha256
    if(Test-Path -LiteralPath $paths.temporary) { throw 'Existing cockpit temporary file retained.' }
    Write-CockpitCameraAtomic $paths $paths.source $script:CockpitCameraOriginalHash $Entry.sha256
}

function Restore-CockpitCamera([string]$Game,[string]$Run,$Entry) {
    Assert-CockpitCameraEntry $Entry
    $paths=Get-CockpitCameraPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'GRID 2 must exit before restoring the cockpit camera.' }
    Assert-CockpitCameraHash $paths.backup $script:CockpitCameraOriginalHash
    if(!(Test-Path -LiteralPath $paths.target -PathType Leaf)) { throw 'Missing installed camera retained for inspection.' }
    $current=(Get-FileHash -LiteralPath $paths.target).Hash
    if($current -notin @($script:CockpitCameraOriginalHash,$Entry.sha256)) { throw 'Foreign installed camera retained for inspection.' }
    if(Test-Path -LiteralPath $paths.temporary) {
        if((Get-FileHash -LiteralPath $paths.temporary).Hash -notin @($script:CockpitCameraOriginalHash,$Entry.sha256)) { throw 'Foreign cockpit temporary file retained.' }
        Remove-Item -LiteralPath $paths.temporary
    }
    if($current -eq $Entry.sha256) { Write-CockpitCameraAtomic $paths $paths.backup $Entry.sha256 $script:CockpitCameraOriginalHash }
    Assert-CockpitCameraHash $paths.target $script:CockpitCameraOriginalHash
}

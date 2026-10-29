param()
$ErrorActionPreference='Stop'
# The launcher shows this line; a refusing Python check prints its specific reason before it.
trap { [Console]::Error.WriteLine('GRID2VR_ERROR|'+($_.Exception.Message -replace '\s+',' ')); exit 1 }
# UTF-8 keeps progress text and non-ASCII paths intact for the launcher and setup.log.
try { [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false) } catch { }
$env:PYTHONIOENCODING='utf-8'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'User-Session.ps1')
. (Join-Path $PSScriptRoot 'Cockpit-Assets.ps1')
if(!(Test-Grid2Package $root)) { throw 'Setup requires an installed package.' }
$game=Get-Grid2GameRoot $root
$data=Get-Grid2DataRoot $root
$null=Assert-PlainPath $data
New-Item -ItemType Directory -Force -Path $data | Out-Null
$python=Get-Grid2Python $root
$converter=Get-Grid2Converter $root
function Progress-Step([int]$Percent,[string]$Message) {
    Write-Output "GRID2VR_PROGRESS|$Percent|$Message"
}
function Test-BankFile([string]$Path,[string]$Expected) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and (Get-FileHash -LiteralPath $Path).Hash -eq $Expected
}
function Test-PreparedBank([string]$Bank,[string]$Folder) {
    # Quiet check: a stale or damaged bank is rebuilt, and a failing rebuild reports its own reason.
    # Cockpit banks are compared with the pinned catalog only; the final verification checks
    # the game's own cameras and reports a changed game file with its path.
    if($Bank -eq 'cockpit-cameras') {
        $catalog=Read-CockpitAssetCatalog $root
        foreach($car in $catalog.cars) {
            foreach($mode in @('HeadOnly','HideDriver')) { if(!(Test-BankFile (Join-Path $Folder "$($car.car)/$mode.bin") $car.$mode)) { return $false } }
        }
        return (Test-BankFile (Join-Path $Folder 'intro.xml') $catalog.intro.sha256)
    }
    if($Bank -eq 'cockpit-start') { return (Test-BankFile (Join-Path $Folder 'startsequence.xml') (Read-CockpitAssetCatalog $root).startsequence.sha256) }
    $module=if($Bank -eq 'startup-movies'){'startup_movies'}else{'practice_entry'}
    & $python -c "import sys; sys.path.insert(0,sys.argv[1]); from $module import verify; verify(sys.argv[2],sys.argv[3])" (Join-Path $root 'tools') $game $Folder 2>$null | Out-Null
    return !$LASTEXITCODE
}
function Remove-PreparedBank([string]$Folder) {
    $resolved=[IO.Path]::GetFullPath($Folder)
    if([IO.Path]::GetDirectoryName($resolved) -ne [IO.Path]::GetFullPath($data)) { throw 'Invalid prepared bank path.' }
    $null=Assert-PlainPath $resolved
    if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked prepared data retained.' }
    Remove-Item -LiteralPath $resolved -Recurse
}
# Each bank is generated from this player's original game. Nothing is downloaded
# or copied from another player's game or career, and no game file is changed.
foreach($bank in @('practice-flow-20261014','startup-movies','cockpit-cameras','cockpit-start')) {
    switch($bank) {
        'practice-flow-20261014' { Progress-Step 16 'Preparing Practice and Race assets…' }
        'startup-movies' { Progress-Step 22 'Preparing intro and logo skipping…' }
        'cockpit-cameras' { Progress-Step 28 'Checking per-car cockpit cameras…' }
        'cockpit-start' { Progress-Step 88 'Preparing the VR cockpit starting view…' }
    }
    $target=Join-Path $data $bank
    if(Test-Path -LiteralPath $target) {
        # A reused bank can go stale: Steam may rewrite the executable with another valid variant
        # (for example Verify integrity), files can be damaged, or a release can change the
        # cockpit catalog. Rebuilding from the originals heals each case without a new folder name.
        if(Test-PreparedBank $bank $target) { continue }
        Remove-PreparedBank $target
    }
    $temporary=Join-Path $data ($bank+'-preparing-'+[guid]::NewGuid().ToString('N'))
    $scriptName=switch($bank){'practice-flow-20261014'{'practice_entry.py'} 'startup-movies'{'startup_movies.py'} 'cockpit-cameras'{'cockpit_assets.py'} 'cockpit-start'{'cockpit_start.py'}}
    $arguments=@('-u',(Join-Path $root "tools/$scriptName"),'--game-root',$game,'--output',$temporary)
    if($bank -ne 'cockpit-start') { $arguments+=@('--converter',$converter) }
    if($bank -eq 'cockpit-cameras') { $arguments+=@('--catalog',(Join-Path $root 'tools/data/cockpit-cars.json'),'--progress') }
    try {
        & $python @arguments
        if($LASTEXITCODE) { throw "Could not prepare $bank; originals are unchanged." }
        Move-Item -LiteralPath $temporary -Destination $target
    } finally {
        $resolved=[IO.Path]::GetFullPath($temporary)
        if([IO.Path]::GetDirectoryName($resolved) -ne [IO.Path]::GetFullPath($data) -or [IO.Path]::GetFileName($resolved) -notmatch '-preparing-[0-9a-f]{32}$') { throw 'Invalid preparation cleanup path.' }
        if(Test-Path -LiteralPath $resolved) {
            $null=Assert-PlainPath $resolved
            if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked preparation data retained.' }
            Remove-Item -LiteralPath $resolved -Recurse
        }
    }
}
& $python (Join-Path $root 'tools/cockpit_start.py') --game-root $game --verify (Join-Path $data 'cockpit-start')
if($LASTEXITCODE) { throw 'Prepared cockpit starting view verification failed.' }
Progress-Step 90 'Verifying head-only cockpit cameras…'
$null=Get-CockpitAssetSelection $root $game 'HeadOnly' '' $true
Progress-Step 94 'Verifying hidden-driver cockpit cameras…'
$null=Get-CockpitAssetSelection $root $game 'HideDriver' '' $true
foreach($scriptName in @('startup_movies.py','practice_entry.py')) {
    # Existing session preflight verifies these exact banks again before deployment.
    $bank=if($scriptName -eq 'startup_movies.py'){'startup-movies'}else{'practice-flow-20261014'}
    if($scriptName -eq 'startup_movies.py') { Progress-Step 97 'Verifying intro and logo skipping…' }
    else { Progress-Step 98 'Verifying Practice and Race assets…' }
    & $python -c "import sys; sys.path.insert(0,sys.argv[1]); from $($scriptName.Replace('.py','')) import verify; verify(sys.argv[2],sys.argv[3])" (Join-Path $root 'tools') $game (Join-Path $data $bank)
    if($LASTEXITCODE) { throw "Prepared $bank verification failed." }
}
Progress-Step 99 'Local game assets verified.'
Write-Output 'Local game assets prepared and verified. Original game files unchanged.'

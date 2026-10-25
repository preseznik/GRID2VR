param()
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'User-Session.ps1')
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
# Each bank is generated from this player's original game. Nothing is downloaded
# or copied from another player's game or career, and no game file is changed.
foreach($bank in @('practice-flow-20261014','startup-movies','cockpit-cameras')) {
    switch($bank) {
        'practice-flow-20261014' { Progress-Step 16 'Preparing Practice and Race assets…' }
        'startup-movies' { Progress-Step 22 'Preparing intro and logo skipping…' }
        'cockpit-cameras' { Progress-Step 28 'Checking per-car cockpit cameras…' }
    }
    $target=Join-Path $data $bank
    if(Test-Path -LiteralPath $target) { continue }
    $temporary=Join-Path $data ($bank+'-preparing-'+[guid]::NewGuid().ToString('N'))
    $scriptName=switch($bank){'practice-flow-20261014'{'practice_entry.py'} 'startup-movies'{'startup_movies.py'} 'cockpit-cameras'{'cockpit_assets.py'}}
    $arguments=@('-u',(Join-Path $root "tools/$scriptName"),'--game-root',$game,'--converter',$converter,'--output',$temporary)
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
. (Join-Path $root 'tools/Cockpit-Assets.ps1')
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

# The caller validates both executable identities before preparing any files.
# Launch revalidates the pair again; backend selection is never a user setting.
function Assert-Grid2LaunchCareer {
    param([ValidateSet('Steam','Direct')][string]$Backend,[string]$CareerProfile='current')
    if($Backend -eq 'Direct' -and $CareerProfile -cne 'current') {
        throw 'This standalone installation supports Current career only. Separate careers require the Steam save provider.'
    }
}

function Get-Grid2LaunchPlan {
    param([Parameter(Mandatory)][string]$Game,
        [Parameter(Mandatory)][ValidateSet('Steam','Direct')][string]$Backend,
        [switch]$NormalLaunch,$SelectedSession,[string]$CareerProfile='current')
    Assert-Grid2LaunchCareer $Backend $CareerProfile
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    if($NormalLaunch -and $SelectedSession) { throw 'Normal launch cannot include a selected session.' }
    $nativeArguments=@()
    if($SelectedSession) {
        # Reconstruct native arguments from the verified selection, never by
        # removing a presumed Steam prefix from caller-supplied command text.
        $mode=$SelectedSession.selection.mode
        if($null -eq $mode) { $mode='benchmark' } # Existing prepared selections may omit the default.
        $flag=switch -CaseSensitive ($mode) {
            'benchmark' { '-benchmark' }
            'attract' { '-benchmarkAttract' }
            'practice' { '-benchmarkAttract' }
            'race' { '-benchmarkAttract' }
            default { throw 'Selected launch mode is unsupported.' }
        }
        $control=if($mode -in @('practice','race')){'human'}else{'ai'}
        if($SelectedSession.control -cne $control) { throw 'Selected launch control differs from its verified mode.' }
        $nativeArguments=@($flag,'GRID2VR.session.xml')
        if(($null -ne $SelectedSession.nativeArguments -and
            (@($SelectedSession.nativeArguments) -join "`0") -cne ($nativeArguments -join "`0")) -or
           ($SelectedSession.nativeConfigurationName -and $SelectedSession.nativeConfigurationName -cne 'GRID2VR.session.xml')) {
            throw 'Selected native launch arguments changed.'
        }
    } elseif(!$NormalLaunch) {
        $nativeArguments=@('-benchmark','example_benchmark.xml')
    }
    if($Backend -eq 'Direct') {
        $executable=Join-Path $gamePath 'grid2_avx.exe'
        $arguments=@($nativeArguments)
        $workingDirectory=$gamePath
    } else {
        $executable=Join-Path (Get-Grid2SteamRoot) 'steam.exe'
        $arguments=@('-applaunch','44350')+$nativeArguments
        $workingDirectory=$null
    }
    return [pscustomobject]@{backend=$Backend;gameRoot=$gamePath;executable=$executable;
        workingDirectory=$workingDirectory;arguments=$arguments;nativeArguments=$nativeArguments}
}

function Start-Grid2Launch {
    param([Parameter(Mandatory)]$Plan)
    if((Get-Grid2LaunchBackend $Plan.gameRoot) -cne $Plan.backend) { throw 'The executable pair changed before launch.' }
    $parameters=@{FilePath=$Plan.executable;WindowStyle='Hidden'}
    if($Plan.arguments.Count) { $parameters.ArgumentList=$Plan.arguments }
    if($Plan.backend -eq 'Direct') { $parameters.WorkingDirectory=$Plan.workingDirectory; $parameters.WindowStyle='Normal' }
    Start-Process @parameters | Out-Null
}

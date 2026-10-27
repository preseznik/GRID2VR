$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Launch-Backend.ps1')
function Check($Value,[string]$Message) { if(!$Value) { throw $Message } }
function Reject([scriptblock]$Action) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    Check $rejected 'Invalid launch was accepted.'
}
function Read-Ast([string]$Name) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$Name"),[ref]$tokens,[ref]$errors)
    Check (!$errors.Count) "$Name has parser errors."
    return $ast
}
# Record the actual Start-Process arguments; never execute a game or Steam.
$script:steamCalls=0; $script:launchCalls=0; $script:backend='Direct'; $script:rejectPair=$false
function Get-Grid2SteamRoot {
    $script:steamCalls++
    if($script:backend -eq 'Direct') { throw 'Standalone unexpectedly required Steam.' }
    return 'C:\Fixture Steam'
}
function Get-Grid2LaunchBackend([string]$Game) {
    if($script:rejectPair) { throw 'Mixed or changed executable pair.' }
    return $script:backend
}
function Start-Process {
    param($FilePath,$WindowStyle,$WorkingDirectory,$ArgumentList)
    $script:launchCalls++
    $script:started=@{}+$PSBoundParameters
}
$game='C:\Fixture Games\GRID 2'
foreach($backend in @('Direct','Steam')) {
    $script:backend=$backend
    $plan=Get-Grid2LaunchPlan -Game $game -Backend $backend -NormalLaunch
    Check (!$plan.nativeArguments.Count) 'Normal launch acquired native arguments.'
    Start-Grid2Launch $plan
    if($backend -eq 'Direct') {
        Check ($script:started.FilePath -ceq (Join-Path $game 'grid2_avx.exe')) 'Standalone executable path changed.'
        Check ($script:started.WorkingDirectory -ceq $game) 'Standalone lost game working directory.'
        Check ($script:started.WindowStyle -ceq 'Normal') 'Interactive standalone game was hidden.'
        Check (!$script:started.ContainsKey('ArgumentList')) 'Normal standalone passed an empty or unwanted argument.'
        Check (!$script:steamCalls) 'Standalone queried Steam.'
        Reject { Get-Grid2LaunchPlan -Game $game -Backend Direct -CareerProfile 'managed-fixture' }
    } else {
        Check (($script:started.ArgumentList -join ' ') -ceq '-applaunch 44350') 'Steam Normal arguments changed.'
        Check (!$script:started.ContainsKey('WorkingDirectory')) 'Steam working directory changed.'
        Check ($script:started.WindowStyle -ceq 'Hidden') 'Steam helper window policy changed.'
    }
    foreach($mode in @('benchmark','attract','practice','race')) {
        $flag=if($mode -eq 'benchmark'){'-benchmark'}else{'-benchmarkAttract'}
        $selection=@{selection=@{mode=$mode};control=$(if($mode -in @('practice','race')){'human'}else{'ai'});
            nativeArguments=@($flag,'GRID2VR.session.xml');nativeConfigurationName='GRID2VR.session.xml'}
        $plan=Get-Grid2LaunchPlan -Game $game -Backend $backend -SelectedSession $selection
        Start-Grid2Launch $plan
        $expected=if($backend -eq 'Steam'){@('-applaunch','44350',$flag,'GRID2VR.session.xml')}else{@($flag,'GRID2VR.session.xml')}
        Check (($script:started.ArgumentList -join ' ') -ceq ($expected -join ' ')) "Wrong $backend $mode arguments."
        Check (($plan.nativeArguments -join ' ') -ceq "$flag GRID2VR.session.xml") 'Native launch receipt differs.'
        Reject { Get-Grid2LaunchPlan -Game $game -Backend $backend -NormalLaunch -SelectedSession $selection }
        $selection.nativeArguments=@($flag,'foreign.xml')
        Reject { Get-Grid2LaunchPlan -Game $game -Backend $backend -SelectedSession $selection }
        $selection.nativeArguments=@($flag,'GRID2VR.session.xml'); $selection.control='invalid'
        Reject { Get-Grid2LaunchPlan -Game $game -Backend $backend -SelectedSession $selection }
    }
    $plan=Get-Grid2LaunchPlan -Game $game -Backend $backend
    Check (($plan.nativeArguments -join ' ') -ceq '-benchmark example_benchmark.xml') 'Legacy benchmark changed.'
    $legacy=@{selection=@{car='s20'};control='ai';nativeArguments=@('-benchmark','GRID2VR.session.xml')}
    $plan=Get-Grid2LaunchPlan -Game $game -Backend $backend -SelectedSession $legacy
    Check (($plan.nativeArguments -join ' ') -ceq '-benchmark GRID2VR.session.xml') 'Omitted legacy mode no longer defaults to benchmark.'
    $before=$script:launchCalls; $script:rejectPair=$true
    Reject { Start-Grid2Launch $plan }
    $script:rejectPair=$false
    Check ($script:launchCalls -eq $before) 'Changed/mixed pair reached process launch.'
    $script:backend=if($backend -eq 'Direct'){'Steam'}else{'Direct'}
    Reject { Start-Grid2Launch $plan }
    Check ($script:launchCalls -eq $before) 'Changed backend reached process launch.'
}

$probe=Read-Ast 'Run-StereoProbe.ps1'
$userdata=$probe.Find({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Clauses[0].Item1.Extent.Text -ceq '$launchBackend -eq ''Steam''' -and $node.Extent.Text.Contains("'userdata'")},$false)
Check ($null -ne $userdata) 'Steam save inspection is not backend-scoped.'
& {
    param($Statement)
    function Get-Grid2SteamRoot { throw 'Standalone enumerated a Steam root.' }
    function Get-ChildItem { throw 'Standalone enumerated saves.' }
    $launchBackend='Direct'; $protectedPaths=@('owned-hardware.xml')
    . ([scriptblock]::Create($Statement))
    Check ($protectedPaths.Count -eq 1) 'Standalone protection scope changed.'
} $userdata.Extent.Text
$theater=$probe.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -ceq '$suppressTheater'},$false)
$Mode='Headset'; $runtimeInfo=@{theaterSuppression=$true}
foreach($launchBackend in @('Direct','Steam')) {
    . ([scriptblock]::Create($theater.Extent.Text))
    Check ($suppressTheater -eq ($launchBackend -eq 'Steam')) 'Theater suppression requires the wrong backend.'
}
foreach($name in @('Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
    $ast=Read-Ast $name
    $guard=$ast.Find({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Assert-Grid2LaunchCareer'},$true)
    $profile=$ast.Find({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Get-LaunchProfile'},$true)
    Check ($guard -and $profile -and $guard.Extent.StartOffset -lt $profile.Extent.StartOffset) "$name resolves managed storage before rejecting unsupported careers."
}
'Launch backends passed: native/Steam arguments, standalone CWD, no Steam save enumeration, early career refusal and pair revalidation before process creation.'

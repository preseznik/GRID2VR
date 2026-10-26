$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Cockpit-Camera.ps1')
$fixture=Join-Path $root ('build/cockpit-recovery-test-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
function Hash-Bytes([byte[]]$Bytes) { return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant() }
$original=[Text.Encoding]::ASCII.GetBytes('original S2000 camera fixture')
$head=[Text.Encoding]::ASCII.GetBytes('head-only seated camera fixture')
$driver=[Text.Encoding]::ASCII.GetBytes('whole-driver-hidden seated camera fixture')
$executable=[Text.Encoding]::ASCII.GetBytes('supported executable fixture')
$script:CockpitCameraOriginalHash=Hash-Bytes $original
$script:CockpitCameraExecutableHash=Hash-Bytes $executable
# Fixture-only executable validator; preserves the transaction's changed-file checks.
function Get-Grid2ExecutableHash([string]$Path,[string]$Variant='Avx') {
    $actual=(Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash.ToLowerInvariant()
    if($actual -ne $script:CockpitCameraExecutableHash) { throw 'Changed fixture executable.' }
    return $actual
}

$script:CockpitCameraHashes=@{HeadOnly=(Hash-Bytes $head);HideDriver=(Hash-Bytes $driver)}
$script:gameRunning=$false
function Get-Process { if($script:gameRunning) { return [pscustomobject]@{Id=42;ProcessName='grid2_avx'} } }
function New-Case([string]$Name) {
    $game=Join-Path $fixture "$Name/game"; $run=Join-Path $fixture "$Name/run"
    [IO.Directory]::CreateDirectory((Join-Path $game 'cars/models/s20')) | Out-Null
    [IO.Directory]::CreateDirectory($run) | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $game 'cars/models/s20/cameras.xml'),$original)
    [IO.File]::WriteAllBytes((Join-Path $game 'grid2_avx.exe'),$executable)
    return @{game=$game;run=$run;paths=(Get-CockpitCameraPaths $game $run)}
}
function Reject([scriptblock]$Action,[string]$Label) {
    $before=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object {$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    if(!$rejected) { throw "Unsafe cockpit operation accepted: $Label" }
    $after=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object {$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
    if(Compare-Object $before $after) { throw "Rejected operation changed files: $Label" }
}
try {
    foreach($mode in @('HeadOnly','HideDriver')) {
        $folder=Join-Path $fixture ('build/cockpit-candidate-20261025/'+$script:CockpitCameraFolders[$mode])
        [IO.Directory]::CreateDirectory($folder) | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $folder 'cameras.xml'),$(if($mode -eq 'HeadOnly'){$head}else{$driver}))
        foreach($phase in @('prepared','installed','install-temp','restore-temp')) {
            $case=New-Case "$mode-$phase"
            $entry=New-CockpitCameraDeployment $case.game $case.run $mode $fixture
            Assert-CockpitCameraHash $case.paths.target $script:CockpitCameraOriginalHash
            Reject {New-CockpitCameraDeployment $case.game $case.run $mode $fixture} 'Repeated preparation'
            if($phase -in @('installed','restore-temp')) { Install-CockpitCamera $case.game $case.run $entry }
            if($phase -eq 'install-temp') { [IO.File]::Copy($case.paths.source,$case.paths.temporary) }
            if($phase -eq 'restore-temp') { [IO.File]::Copy($case.paths.backup,$case.paths.temporary) }
            Restore-CockpitCamera $case.game $case.run $entry
            Restore-CockpitCamera $case.game $case.run $entry
            Assert-CockpitCameraHash $case.paths.target $script:CockpitCameraOriginalHash
            Assert-CockpitCameraHash $case.paths.backup $script:CockpitCameraOriginalHash
            if(Test-Path -LiteralPath $case.paths.temporary) { throw 'Owned temporary file remains.' }
        }
    }
    foreach($file in @('target','source','backup','executable','temporary')) {
        $case=New-Case "install-foreign-$file"
        $entry=New-CockpitCameraDeployment $case.game $case.run HeadOnly $fixture
        [IO.File]::WriteAllText($case.paths[$file],'foreign input')
        Reject {Install-CockpitCamera $case.game $case.run $entry} "Install foreign $file"
    }
    foreach($file in @('target','backup','temporary')) {
        $case=New-Case "restore-foreign-$file"
        $entry=New-CockpitCameraDeployment $case.game $case.run HeadOnly $fixture
        Install-CockpitCamera $case.game $case.run $entry
        [IO.File]::WriteAllText($case.paths[$file],'foreign input')
        Reject {Restore-CockpitCamera $case.game $case.run $entry} "Restore foreign $file"
    }
    $case=New-Case 'guards'
    Reject {New-CockpitCameraDeployment $case.game $case.game HeadOnly $fixture} 'Backup inside installation'
    Reject {Get-CockpitCameraPaths 'relative-game' $case.run} 'Relative installation'
    $script:gameRunning=$true
    Reject {New-CockpitCameraDeployment $case.game $case.run HeadOnly $fixture} 'Running game before prepare'
    $script:gameRunning=$false
    $entry=New-CockpitCameraDeployment $case.game $case.run HeadOnly $fixture
    foreach($bad in @(@{name='../other'},@{car='bac'},@{mode='Other'},@{originalSha256=('a'*64)},@{sha256=('a'*64)})) {
        $changed=$entry.Clone();foreach($key in $bad.Keys){$changed[$key]=$bad[$key]}
        Reject {Install-CockpitCamera $case.game $case.run $changed} 'Invalid deployment entry'
        Reject {Restore-CockpitCamera $case.game $case.run $changed} 'Invalid recovery entry'
    }
    $script:gameRunning=$true
    Reject {Install-CockpitCamera $case.game $case.run $entry} 'Running game before install'
    Reject {Restore-CockpitCamera $case.game $case.run $entry} 'Running game before restore'
    $script:gameRunning=$false

    # Parse real routing and execute its policy guard; no worker or game launch.
    foreach($name in @('Start-UserSession.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        $tokens=$null;$errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$name"),[ref]$tokens,[ref]$errors)
        if($errors.Count) { throw "Invalid worker syntax: $name" }
        $bind=[scriptblock]::Create($ast.ParamBlock.Extent.Text+"`n`$CockpitS2000")
        $arguments=@{}
        if($name -eq 'Run-StereoProbe.ps1') { $arguments.HardwareSettingsFile=Join-Path $root 'tools/stereo-practice-settings.xml' }
        if((& $bind @arguments) -cne 'Off' -or (& $bind @arguments -CockpitS2000 HeadOnly) -cne 'HeadOnly') { throw 'Cockpit opt-in default changed.' }
        if($name -eq 'Start-UserSession.ps1') {
            $guard=$ast.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.Contains('prototype is for the Honda S2000 only')},$false)
            $guard=[scriptblock]::Create($guard.Extent.Text)
            foreach($LaunchMode in @('Practice','Race','Normal')) {
                $CockpitS2000='HeadOnly';$Car='s20'; & $guard
                $Car='pag';$failure=$false;try { & $guard } catch {$failure=$true}
                if($failure -ne ($LaunchMode -ne 'Normal')) { throw 'Unsupported selected car was misrouted.' }
                $CockpitS2000='Off'; & $guard
            }
        } else {
            $guard=$ast.Find({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and
                $n.Clauses[0].Item1.Extent.Text.StartsWith('!$RuntimeSession -or !$UntilExit') -and
                $n.Extent.Text.Contains('Cockpit prototype requires')},$false)
            if(!$guard) { throw 'Worker cockpit scope guard missing.' }
            $guard=[scriptblock]::Create($guard.Extent.Text)
            foreach($caseName in @('s20','normal','wrong-car','missing-session','bounded','no-runtime','input-probe')) {
                $RuntimeSession=$true;$UntilExit=$true;$NormalLaunch=$false;$PlayerControlProbe='Off'
                $selectedSession=@{selection=@{car='s20'}}
                switch($caseName) {
                    'normal' {$NormalLaunch=$true;$selectedSession=$null}
                    'wrong-car' {$selectedSession.selection.car='pag'}
                    'missing-session' {$selectedSession=$null}
                    'bounded' {$UntilExit=$false}
                    'no-runtime' {$RuntimeSession=$false}
                    'input-probe' {$PlayerControlProbe='Human'}
                }
                $failure=$false;try { & $guard } catch {$failure=$true}
                if($failure -ne ($caseName -notin @('s20','normal'))) { throw "$name cockpit scope changed: $caseName" }
            }
        }
    }
    # Execute the actual final launch-parameter selection. Desktop replaces the
    # stereo parameter table, so testing parameter binding alone misses drops.
    $probe=[IO.File]::ReadAllText((Join-Path $root 'tools/Run-StereoProbe.ps1'))
    $start=$probe.IndexOf('    $parameters=@{')
    $end=$probe.IndexOf('    $parameters.CareerProfile=$career.id',$start)
    if($start -lt 0 -or $end -lt 0) { throw 'Diagnostic routing block not found.' }
    $route=[scriptblock]::Create($probe.Substring($start,$end-$start)+"`nreturn `$parameters")
    foreach($PlayerControlOnly in @($false,$true)) {
        foreach($CockpitS2000 in @('Off','HeadOnly','HideDriver')) {
            $Mode=if($PlayerControlOnly){'Desktop'}else{'Headset'}
            $parameters=& $route
            if($parameters.CockpitS2000 -cne $CockpitS2000) { throw "Cockpit option dropped in $Mode routing." }
        }
    }
    Write-Output 'Cockpit transaction: both presets, atomic interruption recovery, foreign data refusal, running-process guards, opt-in defaults, selected-car isolation and Desktop/VR forwarding passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($resolved) -ne [IO.Path]::GetFullPath((Join-Path $root 'build')) -or
       [IO.Path]::GetFileName($resolved) -notmatch '^cockpit-recovery-test-[0-9a-f]{32}$') { throw 'Fixture cleanup boundary invalid.' }
    if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) { throw 'Linked fixture retained.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

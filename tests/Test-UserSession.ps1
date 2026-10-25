$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/User-Session.ps1')
. (Join-Path $root 'tools/Recenter-Key.ps1')
$fixture=Join-Path $root ('artifacts/user-session-test-'+[guid]::NewGuid().ToString('N'))
function Check($Condition,[string]$Message) { if(!$Condition) { throw $Message } }
function Reject([scriptblock]$Action) {
    $before=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object {$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    Check $rejected 'Unsafe operation accepted.'
    $after=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object {$_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash})
    Check (!(Compare-Object $before $after)) 'Rejected operation changed files.'
}
try {
    $report=[xml]'<hardware_settings_info><cpu name="cpu"/><graphics_card name="gpu" native_width="3840" native_height="2160"><resolution width="3840" height="2160"/><capabilities id="same"/></graphics_card></hardware_settings_info>'
    $refresh=[xml]'<hardware_settings_info><cpu name="cpu"/><graphics_card name="gpu" native_width="2560" native_height="1440"><capabilities id="same"/></graphics_card></hardware_settings_info>'
    $beforeReport=$report.OuterXml; $beforeRefresh=$refresh.OuterXml
    Check (Test-HardwareReportRefresh $report $refresh) 'Generated display-mode refresh rejected.'
    Check ($beforeReport -ceq $report.OuterXml) 'Report check modified original.'
    Check ($beforeRefresh -ceq $refresh.OuterXml) 'Report check modified current input.'
    foreach($node in @('cpu','graphics_card','graphics_card/capabilities')) {
        $changed=$refresh.CloneNode($true)
        $changed.SelectSingleNode('/hardware_settings_info/'+$node).SetAttribute('name','different')
        Check (!(Test-HardwareReportRefresh $report $changed)) 'Unrelated hardware report change accepted.'
    }
    Check (!(Test-HardwareReportRefresh $report ([xml]'<hardware_settings_config/>'))) 'Settings XML treated as generated report.'
    $artifacts=Join-Path $fixture 'artifacts'
    $backup=Join-Path $artifacts 'stereo-probe-20261015-000000-000'
    $documents=Join-Path $fixture 'documents'
    New-Item -ItemType Directory -Path $backup,$documents | Out-Null
    Assert-RecenterKey 121 0
    Assert-RecenterKey 82 5
    foreach($binding in @(@(27,0),@(121,8),@(115,2),@(9,2),@(46,3),@(0,0))) {
        Reject {Assert-RecenterKey $binding[0] $binding[1]}
    }
    $files=@(); $i=0
    foreach($name in @('hardware_settings_config.xml','hardware_settings_info.xml')) {
        $target=Join-Path $documents $name
        $source=Join-Path $backup ('protected-'+($i++)+'.bin')
        [IO.File]::WriteAllText($source,'original '+$name)
        [IO.File]::WriteAllText($target,'game-written '+$name)
        $files+=@{path=$target;backup=$source;before=(Get-FileHash -LiteralPath $source).Hash}
    }
    $save=Join-Path $documents 'savegame'
    [IO.File]::WriteAllText($save,'new progress')
    $manifest=Join-Path $backup 'manifest.json'
    Write-SessionJson $manifest @{files=$files}
    Reject {Get-SessionChild $artifacts (Join-Path $fixture 'stereo-probe-1') '^stereo-probe-[0-9-]+$'}
    Reject {Get-SessionChild $artifacts 'relative/path' '.*'}
    # Validate both backups before touching either current settings file.
    [IO.File]::WriteAllText($files[1].backup,'corrupt backup')
    Reject {Restore-UserSettings $backup $documents}
    [IO.File]::WriteAllText($files[1].backup,'original hardware_settings_info.xml')
    $result=Restore-UserSettings $backup $documents
    Check ($result.settingsRestored -and !$result.savesRestored) 'Wrong recovery result.'
    foreach($file in $files) {Check ((Get-FileHash -LiteralPath $file.path).Hash -eq $file.before) 'Original settings not restored.'}
    Check ([IO.File]::ReadAllText($save) -eq 'new progress') 'Recovery touched user progress.'
    Check (@(Get-ChildItem -LiteralPath $backup -Filter 'recovery-current-*').Count -eq 2) 'Changed settings not retained.'
    Restore-UserSettings $backup $documents | Out-Null
    Check (@(Get-ChildItem -LiteralPath $backup -Filter 'recovery-current-*').Count -eq 2) 'Repeated recovery created unnecessary backups.'
    Reject {Send-UserSessionCommand $fixture End}
    $worker=Get-Process -Id $PID
    $session=@{schema=1;workerId=$PID;workerStartedUtc=$worker.StartTime.ToUniversalTime().ToString('o');backup=$backup;run=$null;mode='Headset';runtime='VDXR'}
    $journal=Join-Path $artifacts 'session-pending.json'
    Write-SessionJson $journal $session
    Check (Get-UserSession $fixture).workerAlive 'Actual worker identity rejected.'
    $session.workerStartedUtc='2000-01-01T00:00:00.0000000Z'
    Write-SessionJson $journal $session
    Check (!(Get-UserSession $fixture).workerAlive) 'Reused PID accepted.'
    Reject {Send-UserSessionCommand $fixture StopVR}
    $session.backup=Join-Path $fixture 'stereo-probe-1'
    Write-SessionJson $journal $session
    Reject {Get-UserSession $fixture}
    # Exercise command delivery against a synthetic process/deployment. Only the
    # fixture run receives files; these mocks never inspect or modify the game.
    $run=Join-Path $artifacts 'diagnostic-20261015-000000-000'
    New-Item -ItemType Directory -Path $run | Out-Null
    $session.backup=$backup; $session.run=$run; $session.workerStartedUtc=$worker.StartTime.ToUniversalTime().ToString('o')
    Write-SessionJson $journal $session
    $game='E:\SteamLibrary\steamapps\common\grid 2'
    $fakeHash='a'*64
    $deployment=@{gameRoot=$game;run=$run;files=@(@{name='d3d11.dll';sha256=$fakeHash},@{name='GRID2VR.diagnostic-session';sha256=$fakeHash})}
    Write-SessionJson (Join-Path $artifacts 'diagnostic-pending.json') $deployment
    $identity=@{id=$PID;path=(Join-Path $game 'grid2_avx.exe');createdUtc=$session.workerStartedUtc}
    Write-SessionJson (Join-Path $run 'session.json') @{untilExit=$true;runtimeSession=$true;processes=@($identity)}
    Write-SessionJson (Join-Path $run 'runtime-status.json') @{state=0;failed=$false}
    $fakeProcess=[pscustomobject]@{ExecutablePath=$identity.path;CreationDate=$worker.StartTime}
    function Get-CimInstance([string]$ClassName,[string]$Filter) {
        Check ($ClassName -eq 'Win32_Process' -and $Filter -eq "ProcessId=$PID") 'Unexpected process query.'
        return $fakeProcess
    }
    function Get-FileHash([string]$LiteralPath) {
        if($LiteralPath -in @((Join-Path $game 'd3d11.dll'),(Join-Path $game 'GRID2VR.diagnostic-session'))) { return [pscustomobject]@{Hash=$fakeHash} }
        return Microsoft.PowerShell.Utility\Get-FileHash -LiteralPath $LiteralPath
    }
    foreach($action in @('Recenter','StopVR','End')) { Send-UserSessionCommand $fixture $action | Out-Null }
    foreach($name in @('runtime-recenter.request','runtime-stop.request','session-exit.request')) { Check (Test-Path -LiteralPath (Join-Path $run $name)) 'Command was not delivered.' }
    Reject {Send-UserSessionCommand $fixture CaptureFlicker}
    Write-SessionJson (Join-Path $run 'session.json') @{untilExit=$true;runtimeSession=$true;flickerDiagnostic=$true;processes=@($identity)}
    [IO.File]::WriteAllText((Join-Path $run 'flicker-diagnostic.request'),'opt-in')
    Send-UserSessionCommand $fixture CaptureFlicker | Out-Null
    Check (Test-Path -LiteralPath (Join-Path $run 'flicker-capture.request')) 'Diagnostic capture was not requested.'
    [IO.File]::WriteAllText((Join-Path $run 'flicker-draws.jsonl'),'used')
    Reject {Send-UserSessionCommand $fixture CaptureFlicker}
    $fakeProcess.CreationDate=$worker.StartTime.AddSeconds(-1)
    Reject {Send-UserSessionCommand $fixture End}
    $fakeProcess.CreationDate=$worker.StartTime
    $deployment.files[0].sha256='b'*64
    Write-SessionJson (Join-Path $artifacts 'diagnostic-pending.json') $deployment
    Reject {Send-UserSessionCommand $fixture End}
    $deployment.files[0].sha256=$fakeHash
    Write-SessionJson (Join-Path $artifacts 'diagnostic-pending.json') $deployment
    Write-SessionJson (Join-Path $run 'runtime-status.json') @{state=3;failed=$true}
    Reject {Send-UserSessionCommand $fixture Recenter}
    Check (@(Get-ChildItem -LiteralPath $artifacts -Filter '*.tmp').Count -eq 0) 'Atomic journal left temporary files.'
    Write-Output 'User sessions: recovery, retained progress, corrupt backup refusal, owned paths, PID reuse, command delivery and rejection passed.'
} finally {
    $resolved=Get-SessionChild (Join-Path $root 'artifacts') $fixture '^user-session-test-[0-9a-f]{32}$'
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

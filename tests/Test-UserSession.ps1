$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/User-Session.ps1')
. (Join-Path $root 'tools/Recenter-Key.ps1')
. (Join-Path $root 'tools/Game-Executable.ps1')
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
    # The 4090 report changed this measured value by 104 between launches.
    $memoryReport=[xml]'<hardware_settings_info><systemInfo totalMemory="33289216" rating="3"/><cpu name="cpu"/><graphics_card name="gpu" native_width="3840" native_height="2160"><resolution width="3840" height="2160"/><capabilities id="same"/></graphics_card></hardware_settings_info>'
    $memoryRefresh=[xml]$memoryReport.OuterXml.Replace('33289216','33289112')
    Check (Test-HardwareReportRefresh $memoryReport $memoryRefresh) 'Generated memory measurement refresh rejected.'
    Check ($memoryRefresh.hardware_settings_info.systemInfo.totalMemory -eq '33289112') 'Memory report check modified current input.'
    foreach($value in @('0','-1','invalid','18446744073709551616')) {
        $changed=$memoryRefresh.CloneNode($true)
        $changed.hardware_settings_info.systemInfo.SetAttribute('totalMemory',$value)
        Check (!(Test-HardwareReportRefresh $memoryReport $changed)) 'Invalid memory measurement accepted.'
    }
    foreach($attribute in @('totalMemory','rating')) {
        $changed=$memoryRefresh.CloneNode($true)
        $changed.hardware_settings_info.systemInfo.RemoveAttribute($attribute)
        Check (!(Test-HardwareReportRefresh $memoryReport $changed)) 'Missing hardware report field accepted.'
    }
    $changed=$memoryRefresh.CloneNode($true)
    $changed.hardware_settings_info.systemInfo.SetAttribute('rating','2')
    Check (!(Test-HardwareReportRefresh $memoryReport $changed)) 'Unrelated system information change accepted.'
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
    # Status is polled while the worker replaces it: an open reader never blocks
    # the replace, and a brief foreign lock is retried rather than failing a launch.
    $status=Join-Path $artifacts 'user-session-last.json'
    Write-SessionJson $status @{state='starting';message='one'}
    $reader=[IO.FileStream]::new($status,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite, Delete')
    try { Write-SessionJson $status @{state='starting';message='two'} } finally { $reader.Dispose() }
    Check ((Read-SessionJson $status).message -ceq 'two') 'Replace under an open status reader failed.'
    $holder=Start-ThreadJob -ScriptBlock {
        $lock=[IO.File]::Open($using:status,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        Start-Sleep -Milliseconds 300; $lock.Dispose()
    }
    # Wait until the job holds its read-only-share lock (a writer can no longer open).
    $locked=$false;$deadline=[DateTime]::UtcNow.AddSeconds(5)
    while(!$locked -and [DateTime]::UtcNow -lt $deadline) {
        try { [IO.File]::Open($status,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]'ReadWrite, Delete').Dispose(); Start-Sleep -Milliseconds 5 }
        catch { $locked=$true }
    }
    Check $locked 'Test lock was not taken.'
    Write-SessionJson $status @{state='starting';message='three'}
    $holder | Wait-Job | Remove-Job
    Check ((Read-SessionJson $status).message -ceq 'three') 'Briefly locked status replace was not retried.'
    $lock=[IO.File]::Open($status,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try { Reject {Write-SessionJson $status @{state='starting';message='four'}} } finally { $lock.Dispose() }
    Check ((Read-SessionJson $status).message -ceq 'three') 'Persistently locked status changed.'
    Check (!(Get-ChildItem -LiteralPath $artifacts -Filter '*.tmp')) 'Status temporary file left behind.'
    Remove-Item -LiteralPath $status
    # Renderer load: which d3d11.dll the game process actually uses. This test's own
    # process stands in for the game; its folder copy is loaded only in the last case.
    $gameFolder=Join-Path $fixture 'game';New-Item -ItemType Directory -Path $gameFolder | Out-Null
    $proxy=Join-Path $gameFolder 'd3d11.dll';$systemD3d11=Join-Path ([Environment]::SystemDirectory) 'd3d11.dll'
    Check ((Write-RendererLoad $gameFolder $artifacts $PID 'ABC').reason -ceq 'proxy_missing') 'A removed renderer was not reported.'
    Copy-Item -LiteralPath $systemD3d11 -Destination $proxy
    $proxyHash=(Get-FileHash -LiteralPath $proxy).Hash
    $load=Write-RendererLoad $gameFolder $artifacts $PID $proxyHash
    Check ($load.reason -ceq 'windows_d3d11_loaded' -and !$load.rendererLoaded -and $load.proxyPresent -and $load.proxyUnchanged) 'An unloaded renderer was not reported.'
    # This test process has no "prefer system32 images" policy; a process that has
    # it is reported as prefer_system32_policy instead (verified with a policy-started child).
    Check ($load.preferSystem32Images -eq $false -and ![GRID2VR.GameProcessModules]::PrefersSystem32Images($PID)) 'A prefer-system32 policy was invented.'
    Check ((Write-RendererLoad $gameFolder $artifacts $PID 'ABC').reason -ceq 'proxy_changed') 'A replaced renderer was not reported.'
    $module=[Runtime.InteropServices.NativeLibrary]::Load($proxy)
    try {
        $load=Write-RendererLoad $gameFolder $artifacts $PID $proxyHash
        Check ($load.rendererLoaded -and $load.reason -ceq 'loaded' -and @($load.loadedD3d11) -contains $proxy) 'The loaded renderer was not recognised.'
        $receipt=Get-Content -LiteralPath (Join-Path $artifacts 'renderer-load.json') -Raw | ConvertFrom-Json
        Check ($receipt.rendererLoaded -and $receipt.reason -ceq 'loaded') 'Renderer load receipt is wrong.'
    } finally { [Runtime.InteropServices.NativeLibrary]::Free($module) }
    foreach($reason in 'proxy_missing','proxy_changed','prefer_system32_policy','windows_d3d11_loaded') {
        Check ((Get-RendererLoadMessage $reason) -like 'GRID 2 ran without GRID2VR:*') "No player message for $reason."
    }
    Check ((Get-RendererLoadMessage 'proxy_missing') -like '*Protection history*' -and (Get-RendererLoadMessage 'windows_d3d11_loaded') -like '*Visual C++*') 'Renderer messages do not name the likely fix.'
    foreach($reason in 'loaded','not_inspected','') { Check ($null -eq (Get-RendererLoadMessage $reason)) "A renderer message was produced for $reason." }
    Check ($load.ancestry.Count -ge 1 -and $load.ancestry[0].id -eq $PID -and $load.ancestry[0].preferSystem32Images -eq $false -and $null -eq $load.ruleOrigin) 'Renderer receipt ancestry is wrong.'
    Remove-Item -LiteralPath (Join-Path $artifacts 'renderer-load.json')
    # "Prefer system32 images": a per-program setting forces it with value 1 in bits
    # 60-61 of the first MitigationOptions qword (2 forces it off).
    $on=[byte[]](0,0,0,0,0,0,0,0x10);$off=[byte[]](0,0,0,0,0,0,0,0x20)
    Check ((Test-PreferSystem32Option $on) -and (Test-PreferSystem32Option ([byte[]]($on+(0,0,0,0,0,0,0,0))))) 'A forced PreferSystem32 setting was missed.'
    foreach($options in $off,([byte[]](0,0,0,0,0,0,0,0x30)),([byte[]](0,0,0,0x10)),$null) { Check (!(Test-PreferSystem32Option $options)) 'A PreferSystem32 setting was invented.' }
    Check (!(Test-PreferSystem32Setting '') -and !(Test-PreferSystem32Setting (Join-Path $fixture ('absent-'+[guid]::NewGuid().ToString('N')+'.exe')))) 'An absent program setting was reported.'
    # Origin: the farthest process of the unbroken run that carries the rule.
    $chain=@([ordered]@{name='grid2_avx.exe';preferSystem32Images=$true},[ordered]@{name='grid2.exe';preferSystem32Images=$true},
        [ordered]@{name='steam.exe';preferSystem32Images=$true;exploitProtectionSetting=$false},[ordered]@{name='explorer.exe';preferSystem32Images=$false})
    $origin=Get-PreferSystem32Origin $chain
    Check ($origin.name -ceq 'steam.exe' -and $origin.startedBy -ceq 'explorer.exe') 'The rule origin was not Steam.'
    Check ((Get-PreferSystem32Origin $chain[0..1]).startedBy -eq $null -and $null -eq (Get-PreferSystem32Origin $chain[3..3]) -and $null -eq (Get-PreferSystem32Origin @())) 'Rule origin edges are wrong.'
    Check ($null -eq (Get-PreferSystem32Origin @([ordered]@{name='grid2_avx.exe';preferSystem32Images=$null},$chain[1]))) 'An unreadable game rule produced an origin.'
    Check ((Get-RendererLoadMessage 'prefer_system32_policy' $origin) -like '*Steam passed*Exit Steam completely*') 'The Steam rule message does not say to restart Steam.'
    $origin.exploitProtectionSetting=$true
    Check ((Get-RendererLoadMessage 'prefer_system32_policy' $origin) -like '*Remove the PreferSystem32 exploit-protection setting for steam.exe*') 'The program-setting message does not name the setting.'
    Check ((Get-RendererLoadMessage 'prefer_system32_policy' ([ordered]@{name='helper.exe';exploitProtectionSetting=$false})) -like '*helper.exe passed*Close helper.exe*') 'A non-Steam origin was not named.'
    Check ((Get-PreferSystem32LaunchMessage ([ordered]@{name='steam.exe';exploitProtectionSetting=$false;running=$true})) -like 'GRID2VR did not start GRID 2: Steam is running*Exit Steam completely*') 'The running-Steam refusal is wrong.'
    Check ((Get-PreferSystem32LaunchMessage ([ordered]@{name='pwsh.exe';exploitProtectionSetting=$false;running=$false})) -like '*GRID2VR was started with*Start menu shortcut*') 'The launcher refusal is wrong.'
    # This process has no rule and the fixture has no running Steam: nothing is refused.
    Check ($null -eq (Get-PreferSystem32LaunchBlock Direct $gameFolder $fixture) -and $null -eq (Get-PreferSystem32LaunchBlock Steam $gameFolder $fixture)) 'A launch was refused without the rule.'
    # A real rule: a child turns it on for itself, refuses a launch, and passes it to
    # its own child, which reports the first child as the origin.
    $marker=Join-Path $fixture 'policy-hold';Set-Content -LiteralPath $marker -Value hold
    $policyOut=Join-Path $fixture 'policy-child.json'
    $shell=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $policyChild=Join-Path $fixture 'policy-child.ps1'
    Set-Content -LiteralPath $policyChild -Value @'
param($Tools,$Game,$Out,$Marker)
Add-Type -Namespace Probe -Name Policy -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool SetProcessMitigationPolicy(int policy, ref uint value, UIntPtr size);'
$flags=[uint32]4
if(![Probe.Policy]::SetProcessMitigationPolicy(10,[ref]$flags,[UIntPtr]4)) { exit 2 }
. (Join-Path $Tools 'Game-Executable.ps1')
$block=Get-PreferSystem32LaunchBlock Direct $Game $Game
$hold="while((Test-Path -LiteralPath '$Marker') -and `$i++ -lt 300){Start-Sleep -Milliseconds 100}"
$grandchild=Start-Process -FilePath ([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName) -ArgumentList '-NoProfile','-Command',$hold -PassThru -WindowStyle Hidden
[IO.File]::WriteAllText($Out,(@{grandchild=$grandchild.Id;block=$block} | ConvertTo-Json -Depth 3))
$grandchild.WaitForExit(40000) | Out-Null
'@
    $child=Start-Process -FilePath $shell -ArgumentList '-NoProfile','-File',$policyChild,(Join-Path $root 'tools'),$gameFolder,$policyOut,$marker -PassThru -WindowStyle Hidden
    try {
        for($i=0;$i -lt 300 -and !(Test-Path -LiteralPath $policyOut) -and !$child.HasExited;$i++) { Start-Sleep -Milliseconds 100 }
        Check (Test-Path -LiteralPath $policyOut) "The policy child did not report (exit $(if($child.HasExited){$child.ExitCode}))."
        $policy=Get-Content -LiteralPath $policyOut -Raw | ConvertFrom-Json
        Check ($policy.block.running -eq $false -and $policy.block.exploitProtectionSetting -eq $false -and $policy.block.name -like 'pwsh*') 'A launcher carrying the rule was not refused.'
        $ancestry=@(Get-PreferSystem32Ancestry $policy.grandchild)
        Check ($ancestry.Count -ge 3 -and $ancestry[0].preferSystem32Images -and $ancestry[1].id -eq $child.Id -and $ancestry[1].preferSystem32Images -and $ancestry[2].id -eq $PID -and !$ancestry[2].preferSystem32Images) 'The inherited rule was not traced.'
        $origin=Get-PreferSystem32Origin $ancestry
        Check ($origin.path -ieq $shell -and $origin.startedBy -like 'pwsh*' -and !$origin.exploitProtectionSetting) 'The rule origin was not the first child.'
    } finally {
        Remove-Item -LiteralPath $marker -ErrorAction SilentlyContinue
        $child.WaitForExit(30000) | Out-Null
    }
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

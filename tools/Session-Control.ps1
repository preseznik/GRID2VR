param([ValidateSet('Status','Recenter','StopVR','End','Recover','CaptureFlicker')][string]$Action='Status')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'User-Session.ps1')
$session=Get-UserSession $root
if($Action -eq 'Status') {
    $result=@{pending=[bool]$session;session=$session;status=$null;readiness=$null;resolution=$null;stopped=$false;last=$null}
    $last=Join-Path (Get-Grid2DataRoot $root) 'user-session-last.json'
    if(Test-Path -LiteralPath $last) {
        try {
            $result.last=Read-SessionJson $last
            $worker=Get-Process -Id $result.last.workerId -ErrorAction SilentlyContinue
            $alive=$worker -and $worker.StartTime.ToUniversalTime().Ticks -eq ([datetimeoffset]$result.last.workerStartedUtc).UtcTicks
            $result.last | Add-Member -NotePropertyName workerAlive -NotePropertyValue ([bool]$alive) -Force
        } catch {}
    }
    if($session -and $session.run) {
        foreach($pair in @(@('status','runtime-status.json'),@('readiness','runtime-readiness.json'),@('resolution','scene-resolution.json'))) {
            $file=Join-Path $session.run $pair[1]
            if(Test-Path -LiteralPath $file) { try {$result[$pair[0]]=Read-SessionJson $file} catch {} }
        }
        $result.stopped=Test-Path -LiteralPath (Join-Path $session.run 'runtime-stopped')
    }
    # Never present a previous run's dimensions as a measurement of an active one.
    if(!$session -and $result.last -and $result.last.state -ne 'starting' -and $result.last.resolution) { $result.resolution=$result.last.resolution }
    $result | ConvertTo-Json -Depth 8; return
}
if($Action -ne 'Recover') { Send-UserSessionCommand $root $Action | ConvertTo-Json; return }
$mutex=[Threading.Mutex]::new($false,'Global\GRID2VR.Session')
$locked=$false
try {
try {$locked=$mutex.WaitOne(0)} catch [Threading.AbandonedMutexException] {$locked=$true}
if(!$locked) { throw 'A session worker or recovery is still running.' }
$session=Get-UserSession $root
if(!$session) { throw 'No interrupted user session to recover.' }
if($session.workerAlive -or (Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue)) { throw 'Exit the session worker and GRID 2 before recovery.' }
$pending=Join-Path (Get-Grid2DataRoot $root) 'diagnostic-pending.json'
if(Test-Path -LiteralPath $pending) {
    $deployment=Read-SessionJson $pending
    $run=Get-SessionRun $root $deployment.run
    if($session.run -and $session.run -ne $run) { throw 'Recovery belongs to a different run.' }
    $session.run=$run
    & (Join-Path $PSScriptRoot 'Run-Diagnostic.ps1') -Action Restore | Out-Null
}
$documents=Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games/GRID 2/hardwaresettings'
Restore-UserSettings $session.backup $documents | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $session.backup 'user-settings-recovered.json')
$python=Get-SessionPython $root
if(Test-Path -LiteralPath (Join-Path (Get-Grid2DataRoot $root) 'steamvr-theater-pending.json')) {
    Invoke-SteamVrTheater $python restore $session.backup | Out-Null
}
if($session.run) {
    Complete-InterruptedSessionReceipt $session.run (Get-Grid2GameRoot $root)
    & (Join-Path $PSScriptRoot 'Finish-Diagnostic.ps1') -RunPath $session.run -PythonPath $python | Out-Null
}
Copy-Item -LiteralPath (Join-Path (Get-Grid2DataRoot $root) 'session-pending.json') -Destination (Join-Path $session.backup 'user-session-recovered.json') -Force
Remove-Item -LiteralPath (Join-Path (Get-Grid2DataRoot $root) 'session-pending.json')
@{recovered=$true;backup=$session.backup;run=$session.run} | ConvertTo-Json
} finally {
    if($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}

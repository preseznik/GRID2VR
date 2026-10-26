$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/User-Session.ps1')
$fixture=Join-Path $root ('artifacts/session-logs-test-'+[guid]::NewGuid().ToString('N'))
$previousBase=$env:GRID2VR_USER_BASE
$previousData=$env:GRID2VR_DATA_ROOT
function Check($Condition,[string]$Message) { if(!$Condition) { throw $Message } }
function Reject([scriptblock]$Action) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$true }
    Check $rejected 'Unowned log path accepted.'
}
try {
    # Use the installed layout and its actual per-installation data-root hash.
    $package=Join-Path $fixture 'game/GRID2VR'
    $toolFolder=Join-Path $package 'tools'
    New-Item -ItemType Directory -Path $toolFolder | Out-Null
    [IO.File]::WriteAllText((Join-Path $package 'package.json'),'{}')
    $env:GRID2VR_USER_BASE=Join-Path $fixture 'user-data'
    $data=Get-Grid2DataRoot $package
    $first=New-Grid2LogDirectory $package
    $second=New-Grid2LogDirectory $package
    Check ($first -ne $second) 'Consecutive runs reused a log directory.'
    Check ([IO.Path]::GetDirectoryName($first) -eq (Join-Path $data 'logs')) 'Logs escaped installed user data.'
    Check ((Get-SessionRun $package $first) -eq $first) 'Grouped run rejected.'
    $backup=Join-Path $first 'backup';New-Item -ItemType Directory -Path $backup | Out-Null
    Check ((Get-SessionBackup $package $backup) -eq $backup) 'Grouped backup rejected.'
    foreach($path in @((Join-Path $fixture '20261026-212549-730'),(Join-Path $data '20261026-212549-730'),(Join-Path $data 'logs/invalid'),(Join-Path $first 'nested'))) {
        Reject { Get-SessionRun $package $path }
    }
    Reject { Get-SessionBackup $package (Join-Path $first 'not-backup') }
    $worker=Get-Process -Id $PID
    $state=@{schema=1;workerId=$PID;workerStartedUtc=$worker.StartTime.ToUniversalTime().ToString('o');backup=$backup;run=$first;mode='Headset';runtime='SteamVR'}
    Write-SessionJson (Join-Path $data 'session-pending.json') $state
    Check (Get-UserSession $package).workerAlive 'Grouped active session was rejected.'
    $state.run=$second
    Write-SessionJson (Join-Path $data 'session-pending.json') $state
    Reject { Get-UserSession $package }
    $legacyBackup=Join-Path $data 'stereo-probe-20261026-212549-730'
    $legacyRun=Join-Path $data 'diagnostic-20261026-212549-730'
    New-Item -ItemType Directory -Path $legacyBackup,$legacyRun | Out-Null
    $state.backup=$legacyBackup;$state.run=$legacyRun
    Write-SessionJson (Join-Path $data 'session-pending.json') $state
    Check (Get-UserSession $package).workerAlive 'Legacy interrupted session was rejected.'
    Remove-Item -LiteralPath (Join-Path $data 'session-pending.json')
    Write-UserSessionStatus $package @{state='failed';logDirectory=$first;log=(Join-Path $first 'user-session.log')}
    Check ((Read-SessionJson (Join-Path $first 'user-session.json')).state -eq 'failed') 'Run status was not retained.'
    Check ((Get-FileHash -LiteralPath (Join-Path $first 'user-session.json')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $data 'user-session-last.json')).Hash) 'Latest locator differs from per-run status.'
    # The real launch entry point must preserve a useful log even if preparation
    # fails before selection or a native diagnostic run can exist.
    foreach($name in @('Start-UserSession.ps1','User-Session.ps1','Hardware-Settings.ps1','Paths.ps1')) {
        Copy-Item -LiteralPath (Join-Path $root ('tools/'+$name)) -Destination (Join-Path $toolFolder $name)
    }
    [IO.File]::WriteAllText((Join-Path $toolFolder 'Profiles.ps1'),"function Get-LaunchProfile { throw 'Injected preparation failure.' }")
    [IO.File]::WriteAllText((Join-Path $toolFolder 'Recenter-Key.ps1'),'function Assert-RecenterKey {}')
    [IO.File]::WriteAllText((Join-Path $toolFolder 'Recenter-Device.ps1'),'function Assert-RecenterDevice {}')
    $failure=$null
    try { & (Join-Path $toolFolder 'Start-UserSession.ps1') -Mode VR -LaunchMode Practice | Out-Null } catch { $failure=$_.Exception.Message }
    Check ($failure -eq 'Injected preparation failure.') 'Unexpected preparation result.'
    $last=Read-SessionJson (Join-Path $data 'user-session-last.json')
    Check ($last.state -eq 'failed' -and $last.message -eq $failure) 'Preparation failure status was lost.'
    Check ((Get-Grid2LogDirectory $package $last.logDirectory) -eq [IO.Path]::GetDirectoryName($last.log)) 'Worker log was not grouped.'
    Check ([IO.File]::ReadAllText($last.log).Contains($failure)) 'Preparation failure was not logged.'
    Check ((Read-SessionJson (Join-Path $last.logDirectory 'user-session.json')).message -eq $failure) 'Run folder lacks its failure status.'
    Write-Output 'Session logs: installed layout, unique runs, early failure, status copies, legacy recovery, grouped ownership and path refusal passed.'
} finally {
    $env:GRID2VR_USER_BASE=$previousBase;$env:GRID2VR_DATA_ROOT=$previousData
    $resolved=Get-SessionChild (Join-Path $root 'artifacts') $fixture '^session-logs-test-[0-9a-f]{32}$'
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

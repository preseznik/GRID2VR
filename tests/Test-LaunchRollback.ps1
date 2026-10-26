param([string]$Python='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/User-Session.ps1')
function Check($Condition,[string]$Message) { if(!$Condition) { throw $Message } }
$fixture=Join-Path $root ('artifacts/launch-rollback-test-'+[guid]::NewGuid().ToString('N'))
# Execute the actual launch try/catch/finally. Replace only the game-process
# boundary and SteamVR API with failures; no game or headset can be started.
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/Run-StereoProbe.ps1'),[ref]$tokens,[ref]$errors)
Check (!$errors.Count) 'Run-StereoProbe syntax errors.'
$transaction=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.TryStatementAst] -and $_.Extent.Text.Contains('Invoke-SteamVrTheater $PythonPath begin $backup')})
Check ($transaction.Count -eq 1) 'Launch transaction was not found.'
$boundary="(Join-Path `$PSScriptRoot 'Run-Diagnostic.ps1')"
Check ($transaction[0].Extent.Text.Contains($boundary)) 'Game boundary was not found.'
$body=[scriptblock]::Create($transaction[0].Extent.Text.Replace($boundary,"(Join-Path `$stubRoot 'Run-Diagnostic.ps1')").Replace("(Join-Path `$PSScriptRoot 'Finish-Diagnostic.ps1')","(Join-Path `$stubRoot 'Finish-Diagnostic.ps1')"))
try {
    if($Python) {
        $beforeData=$env:GRID2VR_DATA_ROOT
        try {
            $env:GRID2VR_DATA_ROOT=Join-Path $fixture 'worker-data'
            $workerBackup=Join-Path $env:GRID2VR_DATA_ROOT 'stereo-probe-20261026-214003-560'
            New-Item -ItemType Directory -Path $workerBackup | Out-Null
            # Real CLI, real stderr, no OpenVR connection: an invalid receipt fails first.
            $message=''
            try { Invoke-SteamVrTheater $Python begin (Join-Path $fixture 'wrong-receipt-folder') | Out-Null } catch { $message=$_.Exception.Message }
            Check ($message -like '*SteamVR launch preparation failed*Receipt must be*') 'Worker stderr was lost.'
            Invoke-SteamVrTheater $Python restore $workerBackup | Out-Null
            Check ((Read-SessionJson (Join-Path $workerBackup 'theater-restored.json')).result -eq 'no_pending_change') 'No-change recovery required SteamVR.'
        } finally { $env:GRID2VR_DATA_ROOT=$beforeData }
    }
    $stubRoot=Join-Path $fixture 'stubs';New-Item -ItemType Directory -Path $stubRoot | Out-Null
    # Simulate only the game and evidence-analysis boundaries. The actual outer
    # transaction must restore both settings files and release session ownership.
    [IO.File]::WriteAllText((Join-Path $stubRoot 'Finish-Diagnostic.ps1'),"Write-Output 'Evidence analysis boundary reached.'")
    [IO.File]::WriteAllText((Join-Path $stubRoot 'Run-Diagnostic.ps1'),@'
if(!$afterExit) { throw 'Injected launch failure; no game started.' }
$run=Join-Path $artifacts 'diagnostic-20261026-230024-475'
New-Item -ItemType Directory -Path $run | Out-Null
Write-Output "Diagnostic evidence: $run"
$records=@(foreach($file in $files) {
    $before=(Get-FileHash -LiteralPath $file.path).Hash
    $content=if($file.path.EndsWith('hardware_settings_info.xml')) {
        $report=[IO.File]::ReadAllText($file.path).Replace('33289216','33289112')
        if($scenario -eq 'hardware-identity-change') { $report=$report.Replace('name="gpu"','name="different"') }
        $report
    } else { '<hardware_settings_config gameWritten="true"/>' }
    [IO.File]::WriteAllText($file.path,$content)
    @{path=$file.path;before=$before;after=(Get-FileHash -LiteralPath $file.path).Hash}
})
Write-SessionJson (Join-Path $run 'session.json') @{result='process_exited';documentResults=$records}
if($scenario -eq 'changed-after-exit') { [IO.File]::AppendAllText($files[1].path,'<!-- external change -->') }
if($scenario -eq 'corrupt-backup') { [IO.File]::AppendAllText($files[1].backup,'corrupt') }
'@)
    foreach($scenario in @('before-change','after-theater-change','after-settings-change','restore-failure','normal-exit-memory-refresh','hardware-identity-change','changed-after-exit','corrupt-backup')) {
        $afterExit=$scenario -in @('normal-exit-memory-refresh','hardware-identity-change','changed-after-exit','corrupt-backup')
        $refused=$scenario -in @('hardware-identity-change','changed-after-exit','corrupt-backup')
        $artifacts=Join-Path $fixture $scenario
        $game=Join-Path $artifacts 'game';$documents=Join-Path $artifacts 'documents'
        $backup=Join-Path $artifacts 'stereo-probe-20261026-214003-560'
        New-Item -ItemType Directory -Path $game,$documents,$backup | Out-Null
        $hardwarePaths=@();$files=@();$executables=@()
        foreach($name in @('hardware_settings_config.xml','hardware_settings_info.xml')) {
            $path=Join-Path $documents $name;$hardwarePaths+=$path
            $original=if($afterExit -and $name -eq 'hardware_settings_info.xml') {'<hardware_settings_info><systemInfo totalMemory="33289216"/><cpu name="cpu"/><graphics_card name="gpu" native_width="3840" native_height="2160"><resolution width="3840" height="2160"/></graphics_card></hardware_settings_info>'}else{'<original/>'}
            [IO.File]::WriteAllText($path,$original)
            $saved=Join-Path $backup ('protected-'+$files.Count+'.bin');Copy-Item -LiteralPath $path -Destination $saved
            $files+=@{path=$path;backup=$saved;before=(Get-FileHash -LiteralPath $path).Hash}
        }
        foreach($name in @('grid2.exe','grid2_avx.exe')) {
            $path=Join-Path $game $name;[IO.File]::WriteAllText($path,'inert fixture')
            $executables+=@{path=$path;before=(Get-FileHash -LiteralPath $path).Hash}
        }
        Write-SessionJson (Join-Path $backup 'manifest.json') @{files=$files;executables=$executables}
        $sessionJournal=Join-Path $artifacts 'session-pending.json'
        $userSession=@{schema=1;run=$null;backup=$backup}
        Write-SessionJson $sessionJournal $userSession
        $journalBytes=[IO.File]::ReadAllBytes($sessionJournal)
        $hardwareSettingsFile=Join-Path $artifacts 'temporary-settings.xml'
        [IO.File]::WriteAllText($hardwareSettingsFile,'<hardware_settings_config><graphics_card><resolution width="1280"/></graphics_card></hardware_settings_config>')
        $suppressTheater=$true;$UntilExit=$true;$RuntimeSession=$true;$PlayerControlOnly=$false
        $career=@{id='current'};$cockpitAssets=@();$output=[Collections.Generic.List[object]]::new()
        $calls=[Collections.Generic.List[string]]::new()
        function Invoke-SteamVrTheater([string]$Python,[string]$Action,[string]$Backup) {
            $calls.Add($Action)
            $theater=Join-Path $artifacts 'steamvr-theater-pending.json'
            if($Action -eq 'begin') {
                if($scenario -ne 'before-change') { Write-SessionJson $theater @{owned=$true} }
                if($scenario -in @('before-change','after-theater-change')) { throw 'Injected original SteamVR failure.' }
            } else {
                if($scenario -eq 'restore-failure') { throw 'Injected SteamVR recovery failure.' }
                Remove-Item -LiteralPath $theater
            }
        }
        $failure=''
        try { & $body | Out-Null } catch { $failure=$_.Exception.Message }
        if($afterExit) {
            if($refused) {
                $expected=if($scenario -eq 'hardware-identity-change'){'Hardware report has changes beyond detected memory and display modes; retained current file.'}else{'Settings restoration guard failed; retained current file.'}
                Check ($failure -eq $expected) "Wrong recovery refusal for $scenario : $failure"
                Check (Test-Path -LiteralPath $sessionJournal) 'Unsafe recovery lost session ownership.'
                Check ((Get-FileHash -LiteralPath $files[1].path).Hash -ne $files[1].before) 'Refused hardware report was overwritten.'
            } else {
                Check (!$failure) "Normal exit recovery failed: $failure"
                Check (!(Test-Path -LiteralPath $sessionJournal)) 'Normal exit still blocked retry.'
                Check ((Read-SessionJson (Join-Path $backup 'user-session-restored.json')).run -eq (Join-Path $artifacts 'diagnostic-20261026-230024-475')) 'Completed session identity was not retained.'
                foreach($entry in $files+$executables) { Check ((Get-FileHash -LiteralPath $entry.path).Hash -eq $entry.before) 'Normal exit did not restore original files.' }
                Check ([IO.File]::ReadAllText((Join-Path $backup 'game-written-info.xml')).Contains('33289112')) 'Generated hardware report was not preserved.'
                Check (Test-Path -LiteralPath (Join-Path $backup 'preservation-audit.json')) 'Normal exit preservation audit missing.'
            }
            Check (!(Test-Path -LiteralPath (Join-Path $artifacts 'steamvr-theater-pending.json'))) 'Normal exit left SteamVR changes pending.'
            Write-Output "Session exit recovery passed: $scenario"
            continue
        }
        $expected=if($scenario -eq 'restore-failure'){'Injected SteamVR recovery failure.'}elseif($scenario -eq 'after-settings-change'){'Injected launch failure; no game started.'}else{'Injected original SteamVR failure.'}
        Check ($failure -eq $expected) "Wrong primary error for $scenario : $failure"
        foreach($entry in $files+$executables) { Check ((Get-FileHash -LiteralPath $entry.path).Hash -eq $entry.before) 'Original file was not preserved/restored.' }
        if($scenario -eq 'restore-failure') {
            Check (Test-Path -LiteralPath $sessionJournal) 'Failed recovery lost session ownership.'
            Check (Test-Path -LiteralPath (Join-Path $artifacts 'steamvr-theater-pending.json')) 'Failed recovery lost SteamVR ownership.'
        } else {
            Check (!(Test-Path -LiteralPath $sessionJournal)) 'Successful rollback blocked retry.'
            Check ([Convert]::ToHexString([IO.File]::ReadAllBytes((Join-Path $backup 'user-session-restored.json'))) -eq [Convert]::ToHexString($journalBytes)) 'Original journal was not archived.'
        }
        Check (($calls -join ',') -eq $(if($scenario -eq 'before-change'){'begin'}else{'begin,restore'})) 'Restore called without an owned SteamVR change.'
        Write-Output "Launch rollback passed: $scenario"
    }
} finally {
    $resolved=Get-SessionChild (Join-Path $root 'artifacts') $fixture '^launch-rollback-test-[0-9a-f]{32}$'
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

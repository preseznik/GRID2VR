. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'Hardware-Settings.ps1')
# Shared launcher control and recovery. No game/window input is synthesized.
function Test-HardwareReportRefresh([xml]$Original,[xml]$Current) {
    # This is the game's generated inventory, not hardware_settings_config.xml.
    # A virtual display can change both native size and available display modes.
    $before=$Original.CloneNode($true); $after=$Current.CloneNode($true)
    # GRID 2 refreshes this measured value on startup; even the same machine
    # can report slightly different memory. Identity/capability fields stay exact.
    $systemBefore=$before.hardware_settings_info.systemInfo; $systemAfter=$after.hardware_settings_info.systemInfo
    if($systemBefore -or $systemAfter) {
        if($systemBefore -isnot [Xml.XmlElement] -or $systemAfter -isnot [Xml.XmlElement]) { return $false }
        foreach($system in @($systemBefore,$systemAfter)) {
            $memory=[uint64]0
            if(![uint64]::TryParse($system.GetAttribute('totalMemory'),[ref]$memory) -or !$memory) { return $false }
        }
        $systemAfter.SetAttribute('totalMemory',$systemBefore.GetAttribute('totalMemory'))
    }
    $a=$before.hardware_settings_info.graphics_card; $b=$after.hardware_settings_info.graphics_card
    if($a -isnot [Xml.XmlElement] -or $b -isnot [Xml.XmlElement]) { return $false }
    foreach($attribute in @('native_width','native_height')) {
        if(!$a.HasAttribute($attribute) -or !$b.HasAttribute($attribute)) { return $false }
        $b.SetAttribute($attribute,$a.GetAttribute($attribute))
    }
    foreach($card in @($a,$b)) {
        foreach($mode in @($card.SelectNodes('resolution'))) { $null=$card.RemoveChild($mode) }
    }
    # CPU, GPU identity, capabilities and all other report contents must match.
    return $after.OuterXml -ceq $before.OuterXml
}

function Assert-PlainPath([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path)
    for($part=$full; $part; $part=[IO.Path]::GetDirectoryName($part)) {
        if((Test-Path -LiteralPath $part) -and ((Get-Item -LiteralPath $part -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Linked path retained: $part" }
    }
    return $full
}
function Get-SessionChild([string]$Root,[string]$Path,[string]$Pattern) {
    if(![IO.Path]::IsPathRooted($Path)) { throw 'Session paths must be absolute.' }
    $full=Assert-PlainPath $Path
    $parent=Assert-PlainPath $Root
    if([IO.Path]::GetDirectoryName($full) -ne $parent -or [IO.Path]::GetFileName($full) -notmatch $Pattern) { throw 'Session path is outside its owned directory.' }
    return $full
}
function Get-SessionRun([string]$Root,[string]$Path) {
    $data=Get-Grid2DataRoot $Root
    if([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Path)) -eq [IO.Path]::GetFullPath($data)) {
        return Get-SessionChild $data $Path '^diagnostic-\d{8}-\d{6}-\d{3}$'
    }
    return Get-Grid2LogDirectory $Root $Path
}
function Get-SessionBackup([string]$Root,[string]$Path) {
    $data=Get-Grid2DataRoot $Root
    if([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Path)) -eq [IO.Path]::GetFullPath($data)) {
        return Get-SessionChild $data $Path '^stereo-probe-\d{8}-\d{6}-\d{3}$'
    }
    $run=Get-Grid2LogDirectory $Root ([IO.Path]::GetDirectoryName($Path))
    return Get-SessionChild $run $Path '^backup$'
}
function Read-SessionJson([string]$Path) {
    Assert-PlainPath $Path | Out-Null
    # Share delete: the launcher polls status files while the worker atomically
    # replaces them, and a reader must never make that replace fail.
    $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]'ReadWrite, Delete')
    try {
        if($stream.Length -gt 256KB) { throw 'Session metadata exceeds its bound.' }
        $text=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false),$true).ReadToEnd()
    } finally { $stream.Dispose() }
    return $text | ConvertFrom-Json
}
function Test-SharingViolation($ErrorRecord) {
    for($exception=$ErrorRecord.Exception;$exception;$exception=$exception.InnerException) {
        # ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION
        if($exception -is [IO.IOException] -and $exception.HResult -in @(-2147024864,-2147024863)) { return $true }
    }
    return $false
}
function Write-SessionJson([string]$Path,$Value) {
    $target=Assert-PlainPath $Path
    $temporary=$target+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($Value | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
        if(Test-Path -LiteralPath $target) {
            # Another reader (a scanner, an older tool) may hold the old file
            # briefly; retry only a sharing violation, within about a second.
            for($attempt=1;;$attempt++) {
                try { [IO.File]::Replace($temporary,$target,[NullString]::Value); break }
                catch { if($attempt -ge 40 -or !(Test-SharingViolation $_)) { throw }; Start-Sleep -Milliseconds 25 }
            }
        }
        else { [IO.File]::Move($temporary,$target) }
    } finally { if(Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}
# How to clear Windows' "prefer system32 images" rule from the program that passes
# it to GRID 2 ($Origin: name, exploitProtectionSetting; see Game-Executable.ps1).
function Get-PreferSystem32Fix($Origin) {
    $name=if($Origin -and $Origin.name){$Origin.name}else{'steam.exe'}
    if($Origin -and $Origin.exploitProtectionSetting) { return "Remove the PreferSystem32 exploit-protection setting for $name (it forces this rule), close $name, then launch again." }
    if($name -ieq 'steam.exe') { return 'Exit Steam completely (Steam > Exit), start it again from the Start menu, then launch again.' }
    return "Close $name and start it again normally, then launch again."
}
# Player-facing reason when GRID 2 did not load GRID2VR's d3d11.dll
# (renderer-load.json from Run-Diagnostic); $null when no failure is proven.
function Get-RendererLoadMessage([string]$Reason,$Origin=$null) {
    switch($Reason) {
        'proxy_missing' { return "GRID 2 ran without GRID2VR: GRID2VR's d3d11.dll was removed from the game folder before the game started. This is usually antivirus; check Windows Security > Protection history, allow the file or exclude the game folder, then launch again." }
        'prefer_system32_policy' {
            $source=if($Origin -and $Origin.name -and $Origin.name -ine 'steam.exe'){$Origin.name}else{'Steam'}
            return "GRID 2 ran without GRID2VR: $source passed the Windows rule 'prefer system32 images' to GRID 2, so the game used Windows' own d3d11.dll instead of GRID2VR's. $(Get-PreferSystem32Fix $Origin)"
        }
        'proxy_changed' { return "GRID 2 ran without GRID2VR: GRID2VR's d3d11.dll in the game folder was replaced before the game started. Check for another mod or tool that installs a d3d11.dll, then launch again." }
        'windows_d3d11_loaded' { return "GRID 2 ran without GRID2VR: Windows could not load GRID2VR's d3d11.dll and used its own. Install the latest Microsoft Visual C++ 2015-2022 Redistributable (x86), and check Windows Security > Protection history for a blocked file, then launch again." }
        default { return $null }
    }
}
# Refusal before launch when the program that would start GRID 2 carries the rule
# ($Block from Get-PreferSystem32LaunchBlock).
function Get-PreferSystem32LaunchMessage($Block) {
    $source=if($Block.exploitProtectionSetting){"Windows exploit protection forces the rule 'prefer system32 images' for $($Block.name)"}
        elseif($Block.running){"Steam is running with the Windows rule 'prefer system32 images'"}
        else{"GRID2VR was started with the Windows rule 'prefer system32 images'"}
    $fix=if($Block.exploitProtectionSetting -or $Block.running){Get-PreferSystem32Fix $Block}else{'Close GRID2VR, start it again from its Start menu shortcut, then launch again.'}
    return "GRID2VR did not start GRID 2: $source, so the game would use Windows' own d3d11.dll instead of GRID2VR's. $fix"
}
function Write-UserSessionStatus([string]$Root,$Value) {
    $run=Get-Grid2LogDirectory $Root $Value.logDirectory
    Write-SessionJson (Join-Path $run 'user-session.json') $Value
    Write-SessionJson (Join-Path (Get-Grid2DataRoot $Root) 'user-session-last.json') $Value
}
function Get-SessionPython([string]$Root) {
    return Get-Grid2Python $Root
}
function Invoke-SteamVrTheater([string]$Python,[ValidateSet('begin','restore')][string]$Action,[string]$Backup) {
    $name=if($Action -eq 'begin'){'theater-begin.json'}else{'theater-restored.json'}
    $PSNativeCommandUseErrorActionPreference=$false
    $output=@(& $Python (Join-Path $PSScriptRoot 'steamvr_theater.py') $Action --receipt (Join-Path $Backup $name) 2>&1)
    $code=$LASTEXITCODE
    $details=($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    if($details.Length -gt 8192) { $details=$details.Substring(0,8192) }
    if($code) {
        $message=if($Action -eq 'begin'){'SteamVR launch preparation failed; game not launched.'}else{'SteamVR preference recovery failed. Start SteamVR and choose Restore original files in GRID2VR.'}
        throw ($message+[Environment]::NewLine+$details)
    }
    Write-Output $details
}
function Get-UserSession([string]$Root) {
    $artifacts=Get-Grid2DataRoot $Root
    $path=Join-Path $artifacts 'session-pending.json'
    if(!(Test-Path -LiteralPath $path)) { return $null }
    $session=Read-SessionJson $path
    if($session.schema -ne 1 -or ($session.workerId -isnot [long] -and $session.workerId -isnot [int]) -or
        $session.workerId -le 0 -or $session.mode -notin @('Headset','Desktop') -or $session.runtime -notin @('VDXR','SteamVR')) { throw 'Invalid user session journal.' }
    $session.backup=Get-SessionBackup $Root $session.backup
    if($session.run) {
        $session.run=Get-SessionRun $Root $session.run
        if([IO.Path]::GetFileName($session.backup) -eq 'backup' -and [IO.Path]::GetDirectoryName($session.backup) -ne $session.run) { throw 'Recovery backup belongs to a different run.' }
    }
    $worker=Get-Process -Id $session.workerId -ErrorAction SilentlyContinue
    $alive=$worker -and $worker.StartTime.ToUniversalTime().Ticks -eq ([datetimeoffset]$session.workerStartedUtc).UtcTicks
    $session | Add-Member -NotePropertyName workerAlive -NotePropertyValue ([bool]$alive) -Force
    return $session
}
function Send-UserSessionCommand([string]$Root,[string]$Action) {
    if($Action -notin @('Recenter','StopVR','End','CaptureFlicker')) { throw 'Unknown session command.' }
    $session=Get-UserSession $Root
    if(!$session -or !$session.workerAlive -or !$session.run) { throw 'No active owned session accepts commands.' }
    $deployment=Read-SessionJson (Join-Path (Get-Grid2DataRoot $Root) 'diagnostic-pending.json')
    if($deployment.run -ne $session.run -or $deployment.gameRoot -ne (Get-Grid2GameRoot $Root)) { throw 'Deployment ownership differs.' }
    foreach($name in @('d3d11.dll','GRID2VR.diagnostic-session')) {
        $entry=@($deployment.files | Where-Object name -eq $name)
        if($entry.Count -ne 1 -or $entry[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Deployment identity is incomplete.' }
        $target=Assert-PlainPath (Join-Path $deployment.gameRoot $name)
        if((Get-FileHash -LiteralPath $target).Hash -ne $entry[0].sha256) { throw 'Changed deployment retained.' }
    }
    $receipt=Read-SessionJson (Join-Path $session.run 'session.json')
    if($receipt.untilExit -ne $true -or $receipt.runtimeSession -ne $true) { throw 'The run is not a user session.' }
    $owned=@(foreach($entry in $receipt.processes) {
        if(($entry.id -isnot [long] -and $entry.id -isnot [int]) -or $entry.id -le 0 -or
            $entry.path -notin @((Join-Path $deployment.gameRoot 'grid2.exe'),(Join-Path $deployment.gameRoot 'grid2_avx.exe'))) { throw 'Invalid owned process receipt.' }
        $current=Get-CimInstance Win32_Process -Filter "ProcessId=$([int]$entry.id)"
        if($current -and $current.ExecutablePath -eq $entry.path -and
            $current.CreationDate.ToUniversalTime().Ticks -eq ([datetimeoffset]$entry.createdUtc).UtcTicks) { $current }
    })
    if(!$owned.Count) { throw 'The owned game is no longer running.' }
    if($Action -in @('Recenter','StopVR') -and $session.mode -ne 'Headset') { throw 'This command requires VR mode.' }
    if($Action -in @('Recenter','CaptureFlicker')) {
        $status=Read-SessionJson (Join-Path $session.run 'runtime-status.json')
        if($status.failed -ne $false -or $status.state -ne 0) { throw "$Action requires an active rendering session." }
    }
    if($Action -eq 'CaptureFlicker') {
        if($receipt.flickerDiagnostic -ne $true -or
            !(Test-Path -LiteralPath (Join-Path $session.run 'flicker-diagnostic.request')) -or
            (Test-Path -LiteralPath (Join-Path $session.run 'flicker-draws.jsonl'))) { throw 'Flicker capture requires an unused, explicitly enabled diagnostic session.' }
    }
    $name=@{Recenter='runtime-recenter.request';StopVR='runtime-stop.request';End='session-exit.request';CaptureFlicker='flicker-capture.request'}[$Action]
    $request=Assert-PlainPath (Join-Path $session.run $name)
    if(!(Test-Path -LiteralPath $request)) {
        $file=[IO.File]::Open($request,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $file.Dispose()
    }
    return @{requested=$Action;run=$session.run}
}
function Restore-UserSettings([string]$Backup,[string]$Documents) {
    # Only the two known settings files may be restored. Saves are never rolled
    # back. Explicit recovery retains changed settings before restoring originals.
    $manifest=Read-SessionJson (Join-Path $Backup 'manifest.json')
    $plan=@()
    foreach($name in @('hardware_settings_config.xml','hardware_settings_info.xml')) {
        $target=Assert-HardwareSettingsPath (Join-Path $Documents $name)
        $entries=@($manifest.files | Where-Object path -eq $target)
        if($entries.Count -ne 1) { throw 'Protected settings manifest is incomplete.' }
        $entry=$entries[0]
        $source=Get-SessionChild $Backup $entry.backup '^protected-[0-9]+\.bin$'
        if($entry.before -notmatch '^[0-9a-fA-F]{64}$' -or (Get-FileHash -LiteralPath $source).Hash -ne $entry.before) { throw 'Changed recovery backup retained.' }
        $after=(Get-FileHash -LiteralPath $target).Hash
        $plan+=@{target=$target;source=$source;before=$entry.before;after=$after;name=$name}
    }
    foreach($entry in $plan) {
        if($entry.after -eq $entry.before) { continue }
        Assert-HardwareSettingsPath $entry.target | Out-Null
        if((Get-FileHash -LiteralPath $entry.target).Hash -ne $entry.after) { throw 'Settings changed during recovery.' }
        $saved=Join-Path $Backup ('recovery-current-'+[guid]::NewGuid().ToString('N')+'-'+$entry.name)
        [IO.File]::Copy($entry.target,$saved,$false)
        if((Get-FileHash -LiteralPath $saved).Hash -ne $entry.after) { throw 'Current settings backup failed.' }
        Assert-HardwareSettingsPath $entry.target | Out-Null
        [IO.File]::Copy($entry.source,$entry.target,$true)
        if((Get-FileHash -LiteralPath $entry.target).Hash -ne $entry.before) { throw 'Settings recovery hash mismatch.' }
    }
    return @{settingsRestored=$true;savesRestored=$false;files=$plan}
}

function Complete-InterruptedSessionReceipt([string]$Run,[string]$Game) {
    # An interrupted worker may have restored its deployment before saving its
    # final receipt. Preserve that receipt and record recovery, never a clean exit.
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Exit GRID 2 before completing recovery.' }
    $path=Assert-PlainPath (Join-Path $Run 'session.json')
    $receipt=Read-SessionJson $path
    if($receipt.result -ne 'prepared') { return }
    $deployment=Read-SessionJson (Assert-PlainPath (Join-Path $Run 'deployment-restored.json'))
    if([IO.Path]::GetFullPath($deployment.run) -ne [IO.Path]::GetFullPath($Run) -or
       [IO.Path]::GetFullPath($deployment.gameRoot) -ne [IO.Path]::GetFullPath($Game)) { throw 'Recovery deployment identity mismatch.' }
    foreach($name in @('d3d11.dll','GRID2VR.diagnostic-session','GRID2VR.session.xml')) {
        if(Test-Path -LiteralPath (Join-Path $Game $name)) { throw 'Deployment still present; interrupted receipt retained.' }
    }
    $original=Assert-PlainPath (Join-Path $Run 'session-before-recovery.json')
    $hash=(Get-FileHash -LiteralPath $path).Hash
    if(Test-Path -LiteralPath $original) {
        if((Get-FileHash -LiteralPath $original).Hash -ne $hash) { throw 'Interrupted receipt backup differs.' }
    } else { [IO.File]::Copy($path,$original,$false) }
    $receipt.result='interrupted_session_recovered'
    $receipt | Add-Member -NotePropertyName recovery -NotePropertyValue @{
        originalReceiptSha256=$hash;completedUtc=[DateTime]::UtcNow.ToString('o');processExitKnown=$false
    } -Force
    Write-SessionJson $path $receipt
}

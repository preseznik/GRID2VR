# Runs one AI benchmark lap of an installed converted custom track through the
# guarded GRID2VR session chain (prepared selection -> -mod overlay -> proxy with
# background play, so GRID 2 does not pause when it loses focus), and captures the
# GRID 2 window passively. Never launches GRID 2 bare. Claim the shared game PC first.
param(
    [Parameter(Mandatory)][string]$Build,
    [ValidateRange(120,600)][int]$TimeoutSeconds=600,
    [ValidateRange(3,60)][int]$CaptureSeconds=15,
    [ValidateRange(60,300)][int]$ArmSeconds=150,
    [string]$Car='z28',
    # Release build of the proxy (the pinned RelWithDebInfo candidate is never rebuilt for probes).
    [string]$DiagnosticProxy=(Join-Path $PSScriptRoot '../build/win32/bin/Release/d3d11.dll')
)
$ErrorActionPreference='Stop'
$Build=(Resolve-Path -LiteralPath $Build).Path
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Paths.ps1')
$game=Get-Grid2GameRoot $root
if($Build.StartsWith($game.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'The track build must be outside the game installation.' }
if(Get-Process grid2_avx,grid2 -ErrorAction SilentlyContinue) { throw 'GRID 2 is already running; not starting another session.' }
$cache=Get-Content -LiteralPath (Join-Path $root 'build/win32/CMakeCache.txt') -Raw
if($cache -notmatch '(?m)^_?Python3_EXECUTABLE:(?:FILEPATH|INTERNAL)=(.+)\r?$') { throw 'Configured Python is missing.' }
$python=$Matches[1].Trim()
# The build must be the one installed pack that provides its track id.
$manifest=Get-Content -LiteralPath (Join-Path $Build 'custom-track.json') -Raw | ConvertFrom-Json
$track=@($manifest.tracks)[0].id
$installed=& $python -I -c "import json,sys; sys.path.insert(0, sys.argv[1]); import session_config as s; print(json.dumps(s.custom_tracks()))" $PSScriptRoot | ConvertFrom-Json
$pack=@($installed | Where-Object id -eq $track)
if($pack.Count -ne 1 -or [IO.Path]::GetFullPath($pack[0].pack) -ne $Build) { throw "Custom track $track is not installed from $Build (installed: $($pack.pack -join ', '))." }
$started=Get-Date
$stamp=$started.ToString('yyyyMMdd-HHmmss')
$selection=Join-Path $Build "selection-$stamp"
& $python (Join-Path $PSScriptRoot 'session_config.py') --game-root $game prepare --output $selection --car $Car --track $track --mode benchmark --control ai --laps 1 | Out-Null
if($LASTEXITCODE) { throw 'Custom-track selection could not be prepared; nothing launched.' }
$results=Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\GRID 2\benchmarks'
$rootBefore=Get-ChildItem -LiteralPath $game -File | ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
# Bounded Desktop runtime session: proxy deployed and restored by Run-StereoProbe,
# background-play suppression on, non-rendering (no stereo replay), owned-process
# recovery at the timeout.
$probeLog=Join-Path $Build "session-$stamp.log"
$probeArgs=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'Run-StereoProbe.ps1'),'-Mode','Desktop','-RuntimeSession','-PlayerControlOnly',
    '-BackgroundPlay','Suppress','-SessionDirectory',$selection,'-PythonPath',$python,'-TimeoutSeconds',"$TimeoutSeconds",'-DiagnosticProxy',((Resolve-Path -LiteralPath $DiagnosticProxy).Path),'-GameStartSeconds','120','-CaptureAfterSeconds','1')
$worker=Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $probeArgs -RedirectStandardOutput $probeLog -RedirectStandardError "$probeLog.err" -WindowStyle Hidden -PassThru
# Passive captures of the GRID 2 window only (PrintWindow, no input). Never the
# desktop: that records whatever else the user has open. Rendering is checked from
# these, not assumed from the benchmark result. Bounded: 960 px JPEGs, at most 80.
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace G2VR -Name GameWindow -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
$captures=Join-Path $Build "captures-$stamp"
New-Item -ItemType Directory -Path $captures | Out-Null
$jpeg=[Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
$quality=New-Object Drawing.Imaging.EncoderParameters 1
$quality.Param[0]=New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Quality),70L
function Save-Capture {
    param([Parameter(Mandatory)]$Process)
    if(@(Get-ChildItem -LiteralPath $captures -File).Count -ge 80) { return }
    $Process.Refresh(); $window=$Process.MainWindowHandle
    if($window -eq [IntPtr]::Zero -or ![G2VR.GameWindow]::IsWindowVisible($window)) { return }
    $rect=New-Object G2VR.GameWindow+RECT
    if(![G2VR.GameWindow]::GetClientRect($window,[ref]$rect) -or $rect.Right -lt 64 -or $rect.Bottom -lt 64) { return }
    $full=New-Object Drawing.Bitmap $rect.Right,$rect.Bottom
    $g=[Drawing.Graphics]::FromImage($full); $hdc=$g.GetHdc()
    # PW_CLIENTONLY | PW_RENDERFULLCONTENT: the game's own client area, even when covered.
    $ok=[G2VR.GameWindow]::PrintWindow($window,$hdc,3); $g.ReleaseHdc($hdc); $g.Dispose()
    if(!$ok) { $full.Dispose(); return }
    $h=[int]($rect.Bottom*960/$rect.Right); $small=New-Object Drawing.Bitmap 960,$h
    $g=[Drawing.Graphics]::FromImage($small); $g.DrawImage($full,0,0,960,$h); $g.Dispose(); $full.Dispose()
    $small.Save((Join-Path $captures ('t{0:0000}s.jpg' -f ((Get-Date)-$started).TotalSeconds)),$jpeg,$quality); $small.Dispose()
}
$seen=$null; $result=$null; $nextCapture=Get-Date; $peakVirtualMb=0; $peakPrivateMb=0; $armed=$false; $endedUnarmed=$false
# The session worker owns the game and its recovery; wait for it to finish.
while(!$worker.HasExited) {
    Start-Sleep -Seconds 5
    $process=Get-Process grid2_avx -ErrorAction SilentlyContinue | Where-Object StartTime -ge $started.AddSeconds(-5) | Select-Object -First 1
    if($process -and !$seen) { $seen=$process; Write-Output "GRID 2 running: $($process.Id)" }
    # Never leave an unattended game that would pause when unfocused: if the proxy
    # has not armed background play within ArmSeconds, end this probe's own game.
    if($seen -and !$armed -and !$endedUnarmed) {
        $evidence=Get-Content -LiteralPath $probeLog -ErrorAction SilentlyContinue | Where-Object { $_ -like 'Diagnostic evidence: *' } | Select-Object -Last 1
        $status=if($evidence){Join-Path $evidence.Substring('Diagnostic evidence: '.Length).Trim() 'background-play.json'}else{$null}
        if($status -and (Test-Path -LiteralPath $status)) { try { $armed=(Get-Content -LiteralPath $status -Raw | ConvertFrom-Json).enabled -eq $true } catch {} }
        if($armed) { Write-Output "Background play armed after $([int]((Get-Date)-$seen.StartTime).TotalSeconds) s." }
        elseif(((Get-Date)-$seen.StartTime).TotalSeconds -gt $ArmSeconds -and (Get-Process -Id $seen.Id -ErrorAction SilentlyContinue)) {
            Write-Warning "Background play not armed after $ArmSeconds s; ending this probe's GRID 2 ($($seen.Id))."
            Stop-Process -Id $seen.Id -Force; $endedUnarmed=$true
        }
    }
    # 32-bit game (4 GB address space): track how close a converted track gets.
    if($seen) { $live=Get-Process -Id $seen.Id -ErrorAction SilentlyContinue; if($live) { $peakVirtualMb=[math]::Max($peakVirtualMb,[int]($live.VirtualMemorySize64/1MB)); $peakPrivateMb=[math]::Max($peakPrivateMb,[int]($live.PrivateMemorySize64/1MB)) } }
    if($seen -and (Get-Process -Id $seen.Id -ErrorAction SilentlyContinue) -and (Get-Date) -ge $nextCapture) { try { Save-Capture -Process $seen } catch { Write-Warning "Capture failed: $_" }; $nextCapture=(Get-Date).AddSeconds($CaptureSeconds) }
}
$worker.WaitForExit()
$result=Get-ChildItem -LiteralPath $results -Filter '*.xml' -ErrorAction SilentlyContinue | Where-Object LastWriteTime -ge $started | Sort-Object LastWriteTime | Select-Object -Last 1
$log=if(Test-Path -LiteralPath $probeLog){Get-Content -LiteralPath $probeLog}else{@()}
$run=($log | Where-Object { $_ -like 'Diagnostic evidence: *' } | Select-Object -Last 1)
$run=if($run){$run.Substring('Diagnostic evidence: '.Length).Trim()}else{$null}
$backgroundPlay=$null
if($run -and (Test-Path -LiteralPath (Join-Path $run 'runtime-summary.json'))) { try { $backgroundPlay=(Get-Content -LiteralPath (Join-Path $run 'runtime-summary.json') -Raw | ConvertFrom-Json).backgroundPlayVerification } catch {} }
$outcome=if($result){'result'}elseif($endedUnarmed){'ended-background-play-not-armed'}elseif($seen){'ended-without-result'}else{'never-started'}
$crashes=Get-ChildItem -LiteralPath $game -Filter '*.mdmp' -File | Where-Object LastWriteTime -ge $started | ForEach-Object Name
$rootAfter=Get-ChildItem -LiteralPath $game -File | Where-Object Extension -ne '.mdmp' | ForEach-Object { "$($_.Name)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }
$receipt=[ordered]@{schema=2;build=$Build;track=$track;started=$started.ToString('o');ended=(Get-Date).ToString('o');outcome=$outcome;sessionExit=$worker.ExitCode
    sessionLog=$probeLog;run=$run;backgroundPlayArmed=$armed;backgroundPlayVerification=$backgroundPlay;crashDumps=@($crashes)
    captures=$captures;captureCount=@(Get-ChildItem -LiteralPath $captures -File).Count
    gameSeconds=if($seen){[int](((Get-Date)-$seen.StartTime).TotalSeconds)}else{0}; peakVirtualMb=$peakVirtualMb; peakPrivateMb=$peakPrivateMb
    installationRootChanged=[bool](Compare-Object @($rootBefore | Where-Object { $_ -notmatch '\.mdmp\|' }) @($rootAfter))
    gameStillRunning=[bool](Get-Process grid2_avx,grid2 -ErrorAction SilentlyContinue)}
if($result) {
    Copy-Item -LiteralPath $result.FullName -Destination $Build
    $xml=[xml](Get-Content -LiteralPath $result.FullName -Raw)
    $receipt.result=$result.Name
    $receipt.resultTrack=$xml.benchmark.track.settings.name+'/'+$xml.benchmark.track.settings.route
    $receipt.raceTime=$xml.benchmark.totals.race_time
    $receipt.averageFps=[double]$xml.benchmark.fps_race.av_fps
    $receipt.minimumFps=[double]$xml.benchmark.fps_race.min_fps
}
$receipt | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Build "probe-$stamp.json") -Encoding utf8NoBOM
$receipt | ConvertTo-Json

param(
    [ValidateSet('Stock','ShippedBenchmark','Benchmark')][string]$Mode = 'Stock',
    [string]$GameRoot = 'E:\SteamLibrary\steamapps\common\grid 2',
    [string]$SteamRoot = 'C:\Program Files (x86)\Steam',
    [ValidateRange(30,900)][int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close the existing GRID 2 session first.' }
foreach ($name in @('d3d11.dll','dxgi.dll')) {
    if (Test-Path -LiteralPath (Join-Path $GameRoot $name)) { throw "Stock baseline requires no local $name." }
}
$run = Join-Path $root ('artifacts\baseline-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$documents = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'My Games\GRID 2'
$protected = @($documents)
Get-ChildItem -LiteralPath (Join-Path $SteamRoot 'userdata') -Directory | ForEach-Object {
    $gameData = Join-Path $_.FullName '44350'
    if (Test-Path -LiteralPath $gameData) { $protected += $gameData }
}
$files = @()
for ($i=0; $i -lt $protected.Count; $i++) {
    Get-ChildItem -LiteralPath $protected[$i] -Recurse -File | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($protected[$i], $_.FullName)
        $backup = Join-Path $run ("protected\$i\" + $relative)
        New-Item -ItemType Directory -Path (Split-Path $backup) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $backup
        $files += [ordered]@{path=$_.FullName; backup=$backup; sha256=(Get-FileHash -LiteralPath $backup).Hash; bytes=$_.Length}
    }
}
$manifest = [ordered]@{
    mode=$Mode; startedUtc=[DateTime]::UtcNow.ToString('o'); gameRoot=$GameRoot
    protectedFiles=$files; executables=@(); processObservations=@(); moduleObservations=@()
    elapsedSeconds=0; result='prepared'; settingsModifiedByHarness=$false
}
foreach ($name in @('grid2.exe','grid2_avx.exe')) {
    $p = Join-Path $GameRoot $name
    $manifest.executables += @{name=$name; sha256=(Get-FileHash -LiteralPath $p).Hash}
}
$argsForSteam = '-applaunch 44350'
if ($Mode -eq 'ShippedBenchmark') { $argsForSteam += ' -benchmark example_benchmark.xml' }
if ($Mode -eq 'Benchmark') {
    # Separate files: neither the installed assets nor Documents settings are edited.
    [xml]$settings = Get-Content -LiteralPath (Join-Path $documents 'hardwaresettings\hardware_settings_config.xml') -Raw
    $resolution = $settings.SelectSingleNode('//resolution')
    foreach ($pair in @(@('width','1280'),@('height','720'),@('aspect','16:9'),@('fullscreen','false'),@('vsync','0'),@('multisampling','off'))) {
        $resolution.SetAttribute($pair[0],$pair[1])
    }
    $settingsPath = Join-Path $run 'hardware_settings_config.xml'
    $settings.Save($settingsPath)
    [xml]$benchmark = Get-Content -LiteralPath (Join-Path $GameRoot 'example_benchmark.xml') -Raw
    $benchmark.config.SetAttribute('hardwaresettings',$settingsPath)
    $benchmark.config.SetAttribute('outputfilename','GRID2VR_' + (Split-Path $run -Leaf))
    $benchmark.config.SetAttribute('cycle_camera','false')
    $benchmark.config.SetAttribute('infinite_loop','false')
    $benchmark.config.SetAttribute('skipreplays','true')
    $benchmark.config.track.SetAttribute('number_cars','1')
    $cars = @($benchmark.config.track.SelectNodes('car'))
    foreach ($car in $cars | Select-Object -Skip 1) { $null=$benchmark.config.track.RemoveChild($car) }
    $benchmarkPath = Join-Path $run 'benchmark.xml'
    $benchmark.Save($benchmarkPath)
    $argsForSteam += ' -benchmark "' + $benchmarkPath + '"'
}
$manifest.launchArguments = $argsForSteam
$receipt = Join-Path $run 'session.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receipt
Write-Output "Baseline evidence: $run"
Start-Process -FilePath (Join-Path $SteamRoot 'steam.exe') -ArgumentList $argsForSteam -WindowStyle Hidden | Out-Null
$timer = [Diagnostics.Stopwatch]::StartNew()
$seen = @{}
$lastReport = -10
try {
    while ($timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $running = @(Get-CimInstance Win32_Process -Filter "Name='grid2.exe' OR Name='grid2_avx.exe'")
        foreach ($proc in $running) {
            if (!$seen.ContainsKey([int]$proc.ProcessId)) {
                $seen[[int]$proc.ProcessId]=$true
                $manifest.processObservations += @{id=$proc.ProcessId; parent=$proc.ParentProcessId; path=$proc.ExecutablePath; commandLine=$proc.CommandLine; seenSeconds=$timer.Elapsed.TotalSeconds}
                Write-Output "Observed $($proc.Name), PID $($proc.ProcessId), parent $($proc.ParentProcessId)"
            }
            if ($timer.Elapsed.TotalSeconds - $lastReport -ge 10) {
                try {
                    $p = Get-Process -Id $proc.ProcessId
                    $manifest.moduleObservations += @{id=$p.Id; seconds=$timer.Elapsed.TotalSeconds; workingSet=$p.WorkingSet64; privateBytes=$p.PrivateMemorySize64; modules=@($p.Modules | Where-Object {$_.ModuleName -match 'd3d|dxgi|steam_api|nvapi|bink'} | ForEach-Object {$_.FileName})}
                } catch { $manifest.lastInspectionError=$_.Exception.Message }
            }
        }
        if ($timer.Elapsed.TotalSeconds - $lastReport -ge 10) {
            $lastReport=$timer.Elapsed.TotalSeconds
            $manifest.elapsedSeconds=[math]::Round($timer.Elapsed.TotalSeconds,2)
            $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receipt
        }
        if ($seen.Count -gt 0 -and $running.Count -eq 0) { $manifest.result='process_exited'; break }
        if ($seen.Count -eq 0 -and $timer.Elapsed.TotalSeconds -gt 60) { $manifest.result='no_game_process_within_60_seconds'; break }
        Start-Sleep -Milliseconds 500
    }
    if ($manifest.result -eq 'prepared') { $manifest.result='observation_timeout_process_left_running' }
} finally {
    $manifest.elapsedSeconds=[math]::Round($timer.Elapsed.TotalSeconds,2)
    $manifest.protectedFileResults=@($files | ForEach-Object {
        $current = if (Test-Path -LiteralPath $_.path) {(Get-FileHash -LiteralPath $_.path).Hash} else {$null}
        @{path=$_.path; before=$_.sha256; after=$current; unchanged=($current -eq $_.sha256)}
    })
    $manifest.benchmarkReports=@(& (Join-Path $PSScriptRoot 'Save-BenchmarkReports.ps1') -Documents $documents -Run $run -StartedUtc $manifest.startedUtc)
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receipt
    Write-Output "Result: $($manifest.result); evidence: $receipt"
}

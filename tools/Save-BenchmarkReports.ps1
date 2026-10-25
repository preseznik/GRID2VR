param([string]$Documents,[string]$Run,[datetimeoffset]$StartedUtc)
$folder=Join-Path $Documents 'benchmarks'
if (!(Test-Path -LiteralPath $folder)) { return }
Get-ChildItem -LiteralPath $folder -Filter '*.xml' -File | Where-Object {
    $_.LastWriteTimeUtc.Ticks -ge $StartedUtc.UtcTicks
} | ForEach-Object {
    $destination=Join-Path $Run ('benchmark-' + $_.Name)
    Copy-Item -LiteralPath $_.FullName -Destination $destination
    [xml]$report=Get-Content -LiteralPath $destination -Raw
    [ordered]@{
        source=$_.FullName; copy=$destination; sha256=(Get-FileHash -LiteralPath $destination).Hash
        races=$report.benchmark.totals.races; raceTime=$report.benchmark.totals.race_time
        featureLevel=$report.benchmark.directx11_feature_level; instructionSet=$report.benchmark.instruction_set
        note='Game-generated result; capture overhead and focus pauses may affect this run.'
    }
}

# Isolated candidate only. The normal launcher and its settings are not modified.
param(
    [ValidateSet('VDXR','SteamVR')][string]$XrRuntime='VDXR',
    [ValidateSet('circuits/brands_hatch/route_2','p2p/okutama/route_1')]
    [string]$Track='circuits/brands_hatch/route_2',
    [switch]$Preflight
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$folder=Join-Path $root 'build/visibility-headset-candidate'
$manifest=Get-Content -LiteralPath (Join-Path $folder 'manifest.json') -Raw | ConvertFrom-Json
$proxy=Join-Path $folder 'GRID2VR.dll'
if($manifest.schema -ne 1 -or !$manifest.offlineQualified -or
   $manifest.proxySha256 -notmatch '^[0-9a-f]{64}$' -or
   (Get-FileHash -LiteralPath $proxy).Hash -ne $manifest.proxySha256 -or
   $Track -notin $manifest.qualifiedTracks) {
    throw 'The isolated headset candidate is missing, changed or not qualified for this route.'
}
# Freeze the tested supporting code too: a rebuilt/replaced worker must be qualified
# again before it can deploy the candidate. Paths are restricted to this workspace.
foreach($entry in $manifest.supportFiles) {
    $path=[IO.Path]::GetFullPath((Join-Path $root $entry.path))
    if(!$path.StartsWith($root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
       (Get-FileHash -LiteralPath $path).Hash -ne $entry.sha256) {
        throw 'Candidate support files changed; prepare and qualify a new bundle first.'
    }
}
& (Join-Path $PSScriptRoot 'Start-UserSession.ps1') -Mode VR -XrRuntime $XrRuntime `
    -Car z28 -Track $Track -HeadsetScale 150 -HeadsetVisibilityCandidate `
    -DiagnosticProxy $proxy -Preflight:$Preflight

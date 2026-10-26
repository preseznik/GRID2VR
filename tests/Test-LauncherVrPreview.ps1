$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Launcher-VrPreview.ps1')
$fixture=Join-Path $root ('build/launcher-preview-test-'+[guid]::NewGuid().ToString('N'))
$folder=Join-Path $fixture 'build/visibility-headset-candidate'
New-Item -ItemType Directory -Path $folder | Out-Null
function Reject([scriptblock]$Action,[string]$Message) {
    $rejected=$false
    try { & $Action | Out-Null } catch { if($_.Exception.Message -notlike $Message) { throw }; $rejected=$true }
    if(!$rejected) { throw 'Invalid preview was accepted.' }
}
function Write-Manifest { $script:manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $script:manifestPath -Encoding utf8 }
try {
    $request=@{Root=$fixture}
    Reject { Get-LauncherVrPreviewProxy @request } '*bundle is missing*'
    $manifest=Get-Content (Join-Path $root 'build/visibility-headset-candidate/manifest.json') -Raw | ConvertFrom-Json
    $manifestPath=Join-Path $folder 'manifest.json'
    $proxy=Join-Path $folder 'GRID2VR.dll'
    Copy-Item -LiteralPath (Join-Path $root 'build/visibility-headset-candidate/GRID2VR.dll') -Destination $proxy
    # Synthetic support scripts are data only. No worker or game is invoked.
    $manifest.supportFiles=@()
    foreach($name in @('Paths','Start-UserSession','Run-StereoProbe','Run-Diagnostic','User-Session','Practice-Flow','Instance-Capacity','Launcher-VrPreview','Startup-Movies','Native-Startup','Cinema-Matte','Cockpit-Camera','Cockpit-Assets','Recenter-Device','Driving-Controls','Session-Control','Profiles')) {
        $relative="tools\$name.ps1"; $path=Join-Path $fixture $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
        [IO.File]::WriteAllText($path,'fixture only')
        $manifest.supportFiles+=@{path=$relative;sha256=(Get-FileHash -LiteralPath $path).Hash}
    }
    foreach($relative in @('tools\cockpit_alignment.py','tools\startup_movies.py','tools\data\cockpit-assets.json','artifacts\startup-movies\states.bin')) {
        $path=Join-Path $fixture $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
        [IO.File]::WriteAllText($path,'fixture only')
        $manifest.supportFiles+=@{path=$relative;sha256=(Get-FileHash -LiteralPath $path).Hash}
    }
    foreach($item in @($manifest.shutdownReceiptFix.evidence)+@($manifest.launcherLifecycleFix.evidence)+@($manifest.vrPresentationFix.evidence)+@($manifest.vrPlayabilityFix.evidence)+@($manifest.vrHudFix.evidence)+@($manifest.replayPresentationFix.evidence)+@($manifest.replayUiContinuityFix.evidence)+@($manifest.launchModesControls.evidence)+@($manifest.garageMenuPresentation.evidence)+@($manifest.inputStartupRepair.evidence)+@($manifest.garageFrostedUi.evidence)+@($manifest.normalTransitionRepair.evidence)+@($manifest.cockpitRecenter.evidence)+@($manifest.cockpitExposure.evidence)+@($manifest.instanceBufferOwnership.evidence)+@($manifest.performancePlacement.evidence)+@($manifest.focusCockpitLoader.evidence)+@($manifest.cockpitTexturesLoader.evidence)) {
        $destination=Join-Path $fixture $item.path
        New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
        Copy-Item -LiteralPath (Join-Path $root $item.path) -Destination $destination
    }
    Write-Manifest
    if((Get-LauncherVrPreviewProxy @request) -ne $proxy) { throw 'Wrong preview proxy.' }
    # Headset coverage metadata must never act as a selection allowlist.
    $manifest.qualifiedTracks=@()
    $manifest.raceEndHeadsetAcceptance=$null; $manifest.okutamaHeadsetCheck=$null
    Write-Manifest
    if((Get-LauncherVrPreviewProxy @request) -ne $proxy) { throw 'Historical route coverage still restricts preview.' }
    $bytes=[IO.File]::ReadAllBytes($proxy); [IO.File]::WriteAllText($proxy,'foreign proxy')
    Reject { Get-LauncherVrPreviewProxy @request } '*bundle changed*'
    if([IO.File]::ReadAllText($proxy) -ne 'foreign proxy') { throw 'Foreign proxy modified.' }
    [IO.File]::WriteAllBytes($proxy,$bytes)
    $support=Join-Path $fixture $manifest.supportFiles[0].path
    [IO.File]::WriteAllText($support,'changed')
    Reject { Get-LauncherVrPreviewProxy @request } '*files changed*'
    [IO.File]::WriteAllText($support,'fixture only')
    $allSupport=$manifest.supportFiles; $manifest.supportFiles=@(); Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*manifest is incomplete*'
    $manifest.supportFiles=$allSupport
    $manifest.supportFiles=@($allSupport | Where-Object {$_.path -ne 'tools\Cockpit-Camera.ps1'}); Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*manifest is incomplete*'
    $manifest.supportFiles=$allSupport
    $manifest.supportFiles+=@{path='..\outside.ps1';sha256=('0'*64)}; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*files changed*'
    $manifest.supportFiles=$allSupport
    $manifest.shutdownReceiptFix.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.shutdownReceiptFix.offlineVerified=$true
    $manifest.launcherLifecycleFix.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.launcherLifecycleFix.offlineVerified=$true
    $manifest.vrPresentationFix.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.vrPresentationFix.offlineVerified=$true
    $manifest.vrPlayabilityFix.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.vrPlayabilityFix.offlineVerified=$true
    $manifest.vrHudFix.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.vrHudFix.offlineVerified=$true
    $manifest.replayPresentationFix.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.replayPresentationFix.offlineVerified=$true
    $manifest.replayUiContinuityFix.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.replayUiContinuityFix.offlineVerified=$true
    $manifest.launchModesControls.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.launchModesControls.offlineVerified=$true
    $manifest.garageMenuPresentation.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.garageMenuPresentation.offlineVerified=$true
    $manifest.inputStartupRepair.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.inputStartupRepair.offlineVerified=$true
    $manifest.garageFrostedUi.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.garageFrostedUi.offlineVerified=$true
    $manifest.normalTransitionRepair.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.normalTransitionRepair.offlineVerified=$true
    $manifest.cockpitRecenter.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.cockpitRecenter.offlineVerified=$true
    $manifest.cockpitExposure.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.cockpitExposure.offlineVerified=$true
    $manifest.instanceBufferOwnership.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.instanceBufferOwnership.offlineVerified=$true
    $manifest.performancePlacement.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.performancePlacement.offlineVerified=$true
    $manifest.focusCockpitLoader.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.focusCockpitLoader.offlineVerified=$true; Write-Manifest
    $focusPath=Join-Path $fixture $manifest.focusCockpitLoader.evidence.path
    $focusProof=Get-Content -LiteralPath $focusPath -Raw | ConvertFrom-Json
    $focusProof.proxySha256='0'*64
    $focusProof | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $focusPath -Encoding utf8
    Reject { Get-LauncherVrPreviewProxy @request } '*files changed*'
    $manifest.focusCockpitLoader.evidence.sha256=(Get-FileHash -LiteralPath $focusPath).Hash; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*Focus cockpit qualification*'
    Copy-Item -LiteralPath (Join-Path $root $manifest.focusCockpitLoader.evidence.path) -Destination $focusPath -Force
    $manifest.focusCockpitLoader.evidence.sha256=(Get-FileHash -LiteralPath $focusPath).Hash
    Write-Manifest
    $manifest.cockpitTexturesLoader.offlineVerified=$false; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*lacks qualification*'
    $manifest.cockpitTexturesLoader.offlineVerified=$true; Write-Manifest
    $cockpitPath=Join-Path $fixture $manifest.cockpitTexturesLoader.evidence.path
    $cockpitProof=Get-Content -LiteralPath $cockpitPath -Raw | ConvertFrom-Json
    $cockpitProof.previousProxySha256='0'*64
    $cockpitProof | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $cockpitPath -Encoding utf8
    Reject { Get-LauncherVrPreviewProxy @request } '*files changed*'
    $manifest.cockpitTexturesLoader.evidence.sha256=(Get-FileHash -LiteralPath $cockpitPath).Hash; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*cockpit textures qualification*'
    Copy-Item -LiteralPath (Join-Path $root $manifest.cockpitTexturesLoader.evidence.path) -Destination $cockpitPath -Force
    $manifest.cockpitTexturesLoader.evidence.sha256=(Get-FileHash -LiteralPath $cockpitPath).Hash; Write-Manifest
    $performancePath=Join-Path $fixture $manifest.performancePlacement.evidence.path
    $performanceProof=Get-Content -LiteralPath $performancePath -Raw | ConvertFrom-Json
    $performanceProof.previousProxySha256='0'*64
    $performanceProof | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $performancePath -Encoding utf8
    $manifest.performancePlacement.evidence.sha256=(Get-FileHash -LiteralPath $performancePath).Hash; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*performance-placement qualification*'
    Copy-Item -LiteralPath (Join-Path $root $manifest.performancePlacement.evidence.path) -Destination $performancePath -Force
    $manifest.performancePlacement.evidence.sha256=(Get-FileHash -LiteralPath $performancePath).Hash
    $instancePath=Join-Path $fixture $manifest.instanceBufferOwnership.evidence.path
    $instanceProof=Get-Content -LiteralPath $instancePath -Raw | ConvertFrom-Json
    $instanceProof.previousProxySha256='0'*64
    $instanceProof | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $instancePath -Encoding utf8
    $manifest.instanceBufferOwnership.evidence.sha256=(Get-FileHash -LiteralPath $instancePath).Hash; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*instance-buffer qualification*'
    Copy-Item -LiteralPath (Join-Path $root $manifest.instanceBufferOwnership.evidence.path) -Destination $instancePath -Force
    $manifest.instanceBufferOwnership.evidence.sha256=(Get-FileHash -LiteralPath $instancePath).Hash
    Write-Manifest
    # A claimed fix must match the selected DLL and the retained prior build.
    $fixPath=Join-Path $fixture $manifest.shutdownReceiptFix.evidence.path
    $qualification=Get-Content -LiteralPath $fixPath -Raw | ConvertFrom-Json
    $qualification.previousProxySha256='0'*64
    $qualification | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $fixPath -Encoding utf8
    Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*files changed*'
    $manifest.shutdownReceiptFix.evidence.sha256=(Get-FileHash -LiteralPath $fixPath).Hash; Write-Manifest
    Reject { Get-LauncherVrPreviewProxy @request } '*receipt-fix qualification*'
    Copy-Item -LiteralPath (Join-Path $root $manifest.shutdownReceiptFix.evidence.path) -Destination $fixPath -Force
    $manifest.shutdownReceiptFix.evidence.sha256=(Get-FileHash -LiteralPath $fixPath).Hash
    Write-Manifest
    if((Get-LauncherVrPreviewProxy @request) -ne $proxy) { throw 'Restored bundle rejected.' }
    foreach($override in @(@{Mode='Desktop'},@{DiagnosticProxy='foreign.dll'},@{FlickerDiagnostic=$true},@{HeadsetVisibilityCandidate=$true})) {
        Reject { & (Join-Path $root 'tools/Start-UserSession.ps1') -PreviewVrFixes @override } '*cannot be combined with diagnostic overrides*'
    }
    Write-Output 'Preview integrity: no route-coverage gate; receipt-fix lineage, changed/missing files, escaping paths and conflicting overrides checked; no game opened.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    if([IO.Path]::GetDirectoryName($resolved) -ne (Join-Path $root 'build') -or [IO.Path]::GetFileName($resolved) -notmatch '^launcher-preview-test-[0-9a-f]{32}$') { throw 'Fixture escaped workspace.' }
    if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
    Remove-Item -LiteralPath $resolved -Recurse
}

. (Join-Path $PSScriptRoot 'Paths.ps1')
# Read-only integrity check of the opt-in launcher candidate. Deployment and recovery
# remain owned by the existing user-session worker and diagnostic transaction.
function Get-LauncherVrPreviewProxy {
    param([string]$Root)
    if(Test-Grid2Package $Root) { return Get-Grid2Proxy $Root }
    $expectedProxy='cdd6e68abcd6e53ee15876f86f952b0e808b2fe515fd443ad80fb05f24fa5da8'
    $focusProxy='fa75274d189e5121cc0e35ccbbc2e2df38b1fee3ef3d5dec3a6b73f559c47495'
    $performanceProxy='9fe9e76f825b34650b64d962fc2e115d4fe09c9190eef0decc90362fbd46fe22'
    $instanceProxy='1d3eb5326182bf5e76309fbb2163f8aac98c7546117701ff9486296d09bd978d'
    $exposureProxy='0ec3e7df3dfaf0d01c8f6850b756aa819647cb255e1ab51321bb0517728c3410'
    $recenterProxy='fb4aea9a5f3ea23a981396bd30f7582ae4edef0f6e7e0845b44bbbe392250608'
    $transitionProxy='256f259fe8988b3f49a7089d171f476da58227fcb957273e780a12bce28b9048'
    $frostedProxy='49f7fb6eecfe360d20e09d284b43918e50f4395363820137b2eb7f5d8b88b7d7'
    $inputStartupProxy='21e4cce177b929fc8fe7116892333a0829f9908b344f36b603eba42cb2be3faa'
    $garageProxy='c67416081afbdbeccbc4db93ba3cb24147204eccaf7070a6a4ce0d253f75e937'
    $modesProxy='8f24fbec29726934d0243829a7f3e8e51ea1d3fc3c47fb31877c45f567ea36c3'
    $continuityProxy='66cf4cebce1ccb155728a0015e6b4dba30feb948bfa5cee96278562b8a9ffcac'
    $replayProxy='c341f2327e20afff0a0a8743f18fb99dbb8aa11134b76ced72a3ea0765c315bd'
    $hudProxy='06ce18a859abe7573262a07f8ba06fe9720c8adb6dd9cc76c9c6070499656ab4'
    $playabilityProxy='967dd576d767bb978857b894a9cc71bff5aaa7128a18aae36dec7da919d13cb8'
    $presentationProxy='3efa77e8c4857b224222b420358318e44dcd97c1affb79a772ec03356af6ed22'
    $lifecycleProxy='3453270afadc2d31f0cc79fb04889d924c489de843553efd8b81cdc8a28e645f'
    $receiptProxy='e972968a2b858e13e158dc498e16c0ee3613596bcd1bc7a1ef216253d16de019'
    $headsetProxy='57df0d8904bfa28096be035718909a65d8d9165796465e1378971e6782572c92'
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $folder=Join-Path $Root 'build/visibility-headset-candidate'
    $manifestPath=Join-Path $folder 'manifest.json'
    if(!(Test-Path -LiteralPath $manifestPath) -or (Get-Item -LiteralPath $manifestPath).Length -gt 1MB) { throw 'Preview VR fixes bundle is missing or invalid.' }
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $proxy=Join-Path $folder 'GRID2VR.dll'
    $fix=$manifest.shutdownReceiptFix
    $lifecycle=$manifest.launcherLifecycleFix
    $presentation=$manifest.vrPresentationFix
    $playability=$manifest.vrPlayabilityFix
    $hud=$manifest.vrHudFix
    $replay=$manifest.replayPresentationFix
    $continuity=$manifest.replayUiContinuityFix
    $modes=$manifest.launchModesControls
    $garage=$manifest.garageMenuPresentation
    $inputStartup=$manifest.inputStartupRepair
    $frostedUi=$manifest.garageFrostedUi
    $transition=$manifest.normalTransitionRepair
    $recenter=$manifest.cockpitRecenter
    $exposure=$manifest.cockpitExposure
    $instanceBuffers=$manifest.instanceBufferOwnership
    $performancePlacement=$manifest.performancePlacement
    $focusCockpit=$manifest.focusCockpitLoader
    $cockpitTextures=$manifest.cockpitTexturesLoader
    if($manifest.schema -ne 1 -or $manifest.offlineQualified -ne $true -or $fix.offlineVerified -ne $true -or
       $fix.previousProxySha256 -ne $headsetProxy -or $fix.proxySha256 -ne $receiptProxy -or
       $lifecycle.offlineVerified -ne $true -or $lifecycle.previousProxySha256 -ne $receiptProxy -or $lifecycle.proxySha256 -ne $lifecycleProxy -or
       $presentation.offlineVerified -ne $true -or $presentation.previousProxySha256 -ne $lifecycleProxy -or $presentation.proxySha256 -ne $presentationProxy -or
       $playability.offlineVerified -ne $true -or $playability.previousProxySha256 -ne $presentationProxy -or $playability.proxySha256 -ne $playabilityProxy -or
       $hud.offlineVerified -ne $true -or $hud.previousProxySha256 -ne $playabilityProxy -or $hud.proxySha256 -ne $hudProxy -or
       $replay.offlineVerified -ne $true -or $replay.previousProxySha256 -ne $hudProxy -or $replay.proxySha256 -ne $replayProxy -or
       $continuity.offlineVerified -ne $true -or $continuity.previousProxySha256 -ne $replayProxy -or $continuity.proxySha256 -ne $continuityProxy -or
       $modes.offlineVerified -ne $true -or $modes.previousProxySha256 -ne $continuityProxy -or $modes.proxySha256 -ne $modesProxy -or
       $garage.offlineVerified -ne $true -or $garage.previousProxySha256 -ne $modesProxy -or $garage.proxySha256 -ne $garageProxy -or
       $inputStartup.offlineVerified -ne $true -or $inputStartup.previousProxySha256 -ne $garageProxy -or $inputStartup.proxySha256 -ne $inputStartupProxy -or
       $frostedUi.offlineVerified -ne $true -or $frostedUi.previousProxySha256 -ne $inputStartupProxy -or $frostedUi.proxySha256 -ne $frostedProxy -or
       $transition.offlineVerified -ne $true -or $transition.previousProxySha256 -ne $frostedProxy -or $transition.proxySha256 -ne $transitionProxy -or
       $recenter.offlineVerified -ne $true -or $recenter.previousProxySha256 -ne $transitionProxy -or $recenter.proxySha256 -ne $recenterProxy -or
       $exposure.offlineVerified -ne $true -or $exposure.previousProxySha256 -ne $recenterProxy -or $exposure.proxySha256 -ne $exposureProxy -or
       $instanceBuffers.offlineVerified -ne $true -or $instanceBuffers.previousProxySha256 -ne $exposureProxy -or $instanceBuffers.proxySha256 -ne $instanceProxy -or
       $performancePlacement.offlineVerified -ne $true -or $performancePlacement.previousProxySha256 -ne $instanceProxy -or $performancePlacement.proxySha256 -ne $performanceProxy -or
       $focusCockpit.offlineVerified -ne $true -or $focusCockpit.previousProxySha256 -ne $performanceProxy -or $focusCockpit.proxySha256 -ne $focusProxy -or
       $cockpitTextures.offlineVerified -ne $true -or $cockpitTextures.previousProxySha256 -ne $focusProxy -or $cockpitTextures.proxySha256 -ne $expectedProxy -or
       $manifest.proxySha256 -ne $expectedProxy -or (Get-FileHash -LiteralPath $proxy -ErrorAction Stop).Hash -ne $expectedProxy) {
        throw 'Preview VR fixes bundle changed or lacks qualification.'
    }
    $required=@('tools\Paths.ps1','tools\Start-UserSession.ps1','tools\Run-StereoProbe.ps1','tools\Run-Diagnostic.ps1',
        'tools\User-Session.ps1','tools\Practice-Flow.ps1','tools\Instance-Capacity.ps1','tools\Launcher-VrPreview.ps1',
        'tools\Driving-Controls.ps1','tools\Startup-Movies.ps1','tools\Native-Startup.ps1','tools\Cinema-Matte.ps1','tools\Cockpit-Camera.ps1','tools\Cockpit-Assets.ps1','tools\Recenter-Device.ps1','tools\cockpit_alignment.py','tools\data\cockpit-assets.json','tools\startup_movies.py','tools\Session-Control.ps1','tools\Profiles.ps1','artifacts\startup-movies\states.bin')
    foreach($name in $required) {
        if($name -notin @($manifest.supportFiles.path)) { throw 'Preview VR fixes support manifest is incomplete.' }
    }
    # Historical headset results document coverage; they do not restrict selections.
    if(!$fix.evidence -or !$lifecycle.evidence -or !$presentation.evidence -or !$playability.evidence -or !$hud.evidence -or !$replay.evidence -or !$continuity.evidence -or !$modes.evidence -or !$garage.evidence -or !$inputStartup.evidence -or !$frostedUi.evidence -or !$transition.evidence -or !$recenter.evidence -or !$exposure.evidence -or !$instanceBuffers.evidence -or !$performancePlacement.evidence -or !$focusCockpit.evidence -or !$cockpitTextures.evidence) { throw 'Preview VR fixes qualification evidence is missing.' }
    foreach($entry in @($manifest.supportFiles)+@($fix.evidence)+@($lifecycle.evidence)+@($presentation.evidence)+@($playability.evidence)+@($hud.evidence)+@($replay.evidence)+@($continuity.evidence)+@($modes.evidence)+@($garage.evidence)+@($inputStartup.evidence)+@($frostedUi.evidence)+@($transition.evidence)+@($recenter.evidence)+@($exposure.evidence)+@($instanceBuffers.evidence)+@($performancePlacement.evidence)+@($focusCockpit.evidence)+@($cockpitTextures.evidence)) {
        if(!$entry.path -or $entry.sha256 -notmatch '^[0-9a-f]{64}$') { throw 'Invalid preview VR fixes file record.' }
        $path=[IO.Path]::GetFullPath((Join-Path $Root $entry.path))
        if(!$path.StartsWith($Root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
           (Get-FileHash -LiteralPath $path -ErrorAction Stop).Hash -ne $entry.sha256) {
            throw 'Preview VR fixes support or qualification files changed; requalify the bundle first.'
        }
    }
    $qualified=Get-Content -LiteralPath (Join-Path $Root $fix.evidence.path) -Raw | ConvertFrom-Json
    if($qualified.passed -ne $true -or $qualified.previousProxySha256 -ne $headsetProxy -or
       $qualified.proxySha256 -ne $receiptProxy -or $qualified.renderingFilesUnchanged -ne $true) {
        throw 'Preview VR fixes lack matching receipt-fix qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $lifecycle.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $receiptProxy -or $current.proxySha256 -ne $lifecycleProxy) {
        throw 'Preview VR fixes lack matching launcher lifecycle qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $presentation.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $lifecycleProxy -or $current.proxySha256 -ne $presentationProxy) {
        throw 'Preview VR fixes lack matching MSAA/presentation qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $playability.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $presentationProxy -or $current.proxySha256 -ne $playabilityProxy) {
        throw 'Preview VR fixes lack matching pause/startup/UI qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $hud.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $playabilityProxy -or $current.proxySha256 -ne $hudProxy) {
        throw 'Preview VR fixes lack matching HUD/presentation qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $replay.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $hudProxy -or $current.proxySha256 -ne $replayProxy) {
        throw 'Preview VR fixes lack matching replay presentation qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $continuity.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $replayProxy -or $current.proxySha256 -ne $continuityProxy) {
        throw 'Preview VR fixes lack matching replay UI/startup qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $modes.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $continuityProxy -or $current.proxySha256 -ne $modesProxy) {
        throw 'Preview VR fixes lack matching launcher modes/controls qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $garage.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $modesProxy -or $current.proxySha256 -ne $garageProxy) {
        throw 'Preview VR fixes lack matching garage-menu qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $inputStartup.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $garageProxy -or $current.proxySha256 -ne $inputStartupProxy) {
        throw 'Preview VR fixes lack matching input/startup qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $frostedUi.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $inputStartupProxy -or $current.proxySha256 -ne $frostedProxy) {
        throw 'Preview VR fixes lack matching garage projection qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $transition.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $frostedProxy -or $current.proxySha256 -ne $transitionProxy) {
        throw 'Preview VR fixes lack matching Normal transition qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $recenter.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $transitionProxy -or $current.proxySha256 -ne $recenterProxy) {
        throw 'Preview VR fixes lack matching cockpit/recenter qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $exposure.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $recenterProxy -or $current.proxySha256 -ne $exposureProxy) {
        throw 'Preview VR fixes lack matching cockpit exposure qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $instanceBuffers.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $exposureProxy -or $current.proxySha256 -ne $instanceProxy) {
        throw 'Preview VR fixes lack matching instance-buffer qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $performancePlacement.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $instanceProxy -or $current.proxySha256 -ne $performanceProxy) {
        throw 'Preview VR fixes lack matching performance-placement qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $focusCockpit.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $performanceProxy -or $current.proxySha256 -ne $focusProxy) {
        throw 'Preview VR fixes lack matching Focus cockpit qualification.'
    }
    $current=Get-Content -LiteralPath (Join-Path $Root $cockpitTextures.evidence.path) -Raw | ConvertFrom-Json
    if($current.passed -ne $true -or $current.previousProxySha256 -ne $focusProxy -or $current.proxySha256 -ne $expectedProxy) {
        throw 'Preview VR fixes lack matching cockpit textures qualification.'
    }
    return $proxy
}

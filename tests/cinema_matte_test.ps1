$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools\Cinema-Matte.ps1')
$fixture=Join-Path $root ('artifacts\cinema-recovery-test-'+[guid]::NewGuid().ToString('N'))
$original=[byte[]]::new(724829)
$original[0]=0x51;$original[3000]=0x71;$original[724828]=0xB2
[byte[]]$indices=0,0,0,1,0,2,0,3,0,2,0,1
foreach($offset in @(456040,457099)) { [Array]::Copy($indices,0,$original,$offset,12) }
$positions=@{
    514752='c220000040c00000348ccde2c2200000c0be8cacb48ccde24220000040c00000348ccde242200000c0be8cacb48ccde2'
    516161='c220000040c00000348ccde2c2200000c0c00000b48ccde24220000040c00000348ccde242200000c0c00000b48ccde2'
}
foreach($offset in $positions.Keys) {
    for($i=0;$i -lt 48;$i++) { $original[$offset+$i]=[Convert]::ToByte($positions[$offset].Substring($i*2,2),16) }
}
$replayY=@{
    120095=@('3e0783c0bea9fcb8ba82b0003a2e4000','3e0783c0c28022ebbe45064b3e035987')
    120131=@('be4cccd0000000003ea9fcb8bea9fcb8','c280000000000000428022ebc28022eb')
}
foreach($offset in $replayY.Keys) {
    for($i=0;$i -lt 16;$i++) { $original[$offset+$i]=[Convert]::ToByte($replayY[$offset][0].Substring($i*2,2),16) }
}
# Exact bounded packet/event/ancestor fields from the fingerprinted source.
$popupEdits=@{
    4=@('000b0f55','000b0fc1')
    16477=@('000acefc','000acf68')
    16489=@('0001a188','0001a1f4')
    121405=@('0000018f','000001fb')
    121421=@('00000130','0000019c')
    121462=@('00000156','000001c2')
    121504=@('00000002','00000003')
    121512=@('00000060','00000064')
    121516=@('00000000','3a800000')
    121552=@('40555555','40556555')
    121614=@('00000002','00000003')
    121618=@('000000a0','000000c4')
    121622=@('000000a8','000000d0')
    121626=@('00000000','3a800000')
    121662=@('40555555','40556555')
    121724=@('00000002','00000003')
    121728=@('000000e8','00000130')
    121732=@('000000f0','0000013c')
    121736=@('00000000','3a800000')
    121772=@('40555555','40556555')
    248467=@('00000000','3a800000')
    248479=@('40555555','40556555')
    257777=@('3e64b17e','3e65b17e')
    684626=@('bec96920','c2800000')
}
$popupKeys=@{
    121516='00000000bf000000c51c264ec4d035beb451d99a3f3fafd0000000000000000000000000'
    121626='00000000bf000000c51c264ec4d035beb451d99ac2800000427e6d2eb580000000000000'
    121736='00000000bf000000c51c264ec4d035beb451d99abe3b7e9d000000000000000000000000'
}
$nativeKeys=@{
    121516='00000000bf000000c51c264ec4d035beba2ee0003f3fafd000000000000000000000000040555555beaaaaabbd97b426bde38e3a40554fdd3f3fafd0000000000000000000000000'
    121626='00000000bf000000c51c264ec4d035beba2ee000bec96920410add263b2ac000bae3a00040555555beaaaaabbd97b426bde38e3a40554fdd4104956c00000000c10add26410add26'
    121736='00000000bf000000c51c264ec4d035beba2ee000be3b7e9d3410000033400000b300000040555555beaaaaabbd97b426bde38e3a40554fddbe3b7e9300000000b410000034100000'
}
# packet589 sponsor anim[y] keys (timing + values) and the component's static anim Y.
$sponsorKeys='00000000bf000000c51c264ec4d035beba2ee000bf06493f3ea00b6c3f22ca9bbed90e2440555555beaaaaabbd97b426bde38e3a40554fdd0000000000000000bea00b6b3ea00b6b'
$sponsorY=@{
    23674=@('bf06493f3ea00b6c3f22ca9bbed90e24','c280000042188d7e429b2bc3c24ee503')
    622018=@('bf06493f','c2800000')
}
function Put-Hex([byte[]]$Bytes,[int]$Offset,[string]$Hex) {
    for($i=0;$i -lt $Hex.Length/2;$i++) { $Bytes[$Offset+$i]=[Convert]::ToByte($Hex.Substring(2*$i,2),16) }
}
foreach($offset in $nativeKeys.Keys) { Put-Hex $original $offset $nativeKeys[$offset] }
foreach($offset in $popupEdits.Keys) { Put-Hex $original $offset $popupEdits[$offset][0] }
Put-Hex $original 23654 $sponsorKeys
Put-Hex $original 622018 $sponsorY[622018][0]
$modified=[byte[]]$original.Clone()
foreach($offset in @(514752,516161)) { [Array]::Clear($modified,$offset,48) }
$verticesOnly=[byte[]]$modified.Clone()
foreach($offset in $replayY.Keys) {
    for($i=0;$i -lt 16;$i++) { $modified[$offset+$i]=[Convert]::ToByte($replayY[$offset][1].Substring($i*2,2),16) }
}
$replayOnly=[byte[]]$modified.Clone()
foreach($offset in $popupEdits.Keys) { Put-Hex $modified $offset $popupEdits[$offset][1] }
foreach($offset in @($popupKeys.Keys | Sort-Object -Descending)) {
    $next=[byte[]]::new($modified.Length+36)
    [Array]::Copy($modified,0,$next,0,$offset)
    Put-Hex $next $offset $popupKeys[$offset]
    [Array]::Copy($modified,$offset,$next,$offset+36,$modified.Length-$offset)
    $modified=$next
}
$popupOnly=[byte[]]$modified.Clone()
# The popup inserts precede the static anim Y, which moves by 3*36 bytes.
Put-Hex $modified 23674 $sponsorY[23674][1]
Put-Hex $modified 622126 $sponsorY[622018][1]
$indexOnly=[byte[]]$original.Clone()
foreach($offset in @(456040,457099)) { [Array]::Clear($indexOnly,$offset,12) }
$executable=[Text.Encoding]::UTF8.GetBytes('verified executable fixture')
function Hash-Bytes([byte[]]$Bytes) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($Bytes)).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
}
function Read-BeFloat([byte[]]$Bytes,[int]$Offset) {
    [byte[]]$word=$Bytes[$Offset..($Offset+3)];[Array]::Reverse($word)
    return [double][BitConverter]::ToSingle($word,0)
}
function Read-YPolynomial([byte[]]$Bytes,[int]$Offset,[double]$T) {
    $value=0.0
    for($i=3;$i -ge 0;$i--) { $value=$value*$T+(Read-BeFloat $Bytes ($Offset+4*$i)) }
    return $value
}
function Read-NativeValuePolynomial([byte[]]$Bytes,[int]$Offset,[single]$T) {
    # B1 evaluates ((u^3*c3 + u^2*c2) + u*c1) + c0 with a float
    # rounding after every operation, rather than double/Horner association.
    [single]$square=$T*$T;[single]$cube=$square*$T
    [single]$cubic=$cube*[single](Read-BeFloat $Bytes ($Offset+12))
    [single]$quadratic=$square*[single](Read-BeFloat $Bytes ($Offset+8))
    [single]$linear=$T*[single](Read-BeFloat $Bytes ($Offset+4))
    [single]$sum=$cubic+$quadratic;$sum=[single]($sum+$linear)
    return [single]($sum+[single](Read-BeFloat $Bytes $Offset))
}
# The production helper still enforces its fingerprints; use synthetic bytes in
# this isolated test rather than distributing a proprietary game asset fixture.
$script:CinemaOriginalHash=Hash-Bytes $original
$script:CinemaModifiedHash=Hash-Bytes $modified
$script:CinemaIndexOnlyHash=Hash-Bytes $indexOnly
$script:CinemaVerticesOnlyHash=Hash-Bytes $verticesOnly
$script:CinemaReplayOnlyHash=Hash-Bytes $replayOnly
$script:CinemaPopupHash=Hash-Bytes $popupOnly
$script:CinemaExecutableHash=Hash-Bytes $executable
# Fixture-only executable validator; preserves the transaction's changed-file checks.
function Get-Grid2ExecutableHash([string]$Path,[string]$Variant='Avx') {
    $actual=(Get-FileHash -LiteralPath $Path -ErrorAction Stop).Hash.ToLowerInvariant()
    if($actual -ne $script:CinemaExecutableHash) { throw 'Changed fixture executable.' }
    return $actual
}

function New-Case([string]$Name) {
    $game=Join-Path $fixture "$Name\game"
    $run=Join-Path $fixture "$Name\run"
    New-Item -ItemType Directory -Path (Join-Path $game 'frontend\databases'),$run | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $game 'frontend\databases\overlayPersistent.pssg'),$original)
    [IO.File]::WriteAllBytes((Join-Path $game 'grid2_avx.exe'),$executable)
    return @{game=$game;run=$run;paths=(Get-CinemaMattePaths $game $run)}
}
function Assert-Rejected([scriptblock]$Action,[string]$Label) {
    $before=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    $rejected=$false
    try { & $Action } catch { $rejected=$true }
    if(!$rejected) { throw "$Label was accepted" }
    $after=@(Get-ChildItem -LiteralPath $fixture -Recurse -File | ForEach-Object { $_.FullName+':'+(Get-FileHash -LiteralPath $_.FullName).Hash })
    if(Compare-Object $before $after) { throw "$Label mutated a fixture before rejection" }
}
try {
    $case=New-Case 'preflight'
    Assert-CinemaMatteOriginal $case.game
    Assert-Rejected { New-CinemaMatteDeployment $case.game $case.game } 'Backup inside installation'
    [IO.File]::WriteAllText($case.paths.target,'foreign asset')
    Assert-Rejected { Assert-CinemaMatteOriginal $case.game } 'Foreign preflight asset'
    foreach($phase in @('before-install','after-install','install-temp','restore-temp')) {
        $case=New-Case $phase
        $entry=New-CinemaMatteDeployment $case.game $case.run
        Assert-CinemaHash $case.paths.source $script:CinemaModifiedHash
        $generated=[IO.File]::ReadAllBytes($case.paths.source)
        foreach($offset in @(456148,457207)) {
            if([BitConverter]::ToString($generated,$offset,12) -cne [BitConverter]::ToString($indices)) { throw 'Original quad indices changed.' }
        }
        foreach($offset in @(514860,516269)) {
            if(@($generated[$offset..($offset+47)] | Where-Object { $_ -ne 0 }).Count) { throw 'Cinema quad retains area.' }
        }
        if([BitConverter]::ToString($generated,120095,4) -cne [BitConverter]::ToString($original,120095,4)) { throw 'Replay ON endpoint changed.' }
        if([BitConverter]::ToString($generated,120131,4) -cne 'C2-80-00-00') { throw 'Replay OFF endpoint was not moved below the canvas.' }
        $on=Read-BeFloat $original 120095;$off=Read-BeFloat $original 120131
        $scale=(-64.0-$on)/($off-$on);$previous=$on
        for($step=0;$step -le 100;$step++) {
            $t=$step/100.0
            $native=Read-YPolynomial $original 120095 $t
            $actual=Read-YPolynomial $generated 120095 $t
            if([Math]::Abs($actual-($on+$scale*($native-$on))) -gt 0.00002 -or $actual -gt $previous+0.00002) { throw 'Replay exit travel changed its native curve shape or reverses direction.' }
            if((Read-YPolynomial $generated 120131 $t) -gt -50.0) { throw 'Replay OFF continuation can re-enter the canvas.' }
            $previous=$actual
        }
        if($generated.Length -ne 724937) { throw 'Rebuilt popup asset has wrong length.' }
        # Original coefficient arrays remain byte-identical, including inverse
        # time and terminal values. All channels have a reachable hidden time0.
        for($axis=0;$axis -lt 3;$axis++) {
            $originalKey=121516+110*$axis
            $hiddenKey=$originalKey+36*$axis
            if((Read-BeFloat $generated $hiddenKey) -ne 0) { throw 'Hidden key is unreachable at default frame0.' }
            if((Read-BeFloat $generated ($hiddenKey+36)) -ne 0.0009765625) { throw 'Visible origin was not translated exactly.' }
            for($key=0;$key -lt 2;$key++) {
                if([BitConverter]::ToString($generated,($hiddenKey+36+36*$key+4),32) -cne [BitConverter]::ToString($original,($originalKey+36*$key+4),32)) { throw 'Original visible popup curve changed.' }
            }
        }
        if((Read-BeFloat $generated 121682) -ne -64 -or (Read-BeFloat $generated 684734) -ne -64) { throw 'Static/default hidden pose mismatch.' }
        if([Math]::Abs((Read-NativeValuePolynomial $generated 121682 1)-(Read-BeFloat $original 121646)) -gt 0.000002) { throw 'Hidden segment does not meet native visible origin.' }
        $previous=-64.0
        for($step=0;$step -le 1000;$step++) {
            $actual=Read-NativeValuePolynomial $generated 121682 ([single]($step/1000.0))
            if($actual -lt $previous -or $actual -lt -64 -or $actual -gt -0.393378) { throw 'Hidden interval overshoots or reverses.' }
            $previous=$actual
        }
        # Sponsor objective bar: key0 values scale about the exact ON endpoint;
        # its timing words and key1 stay native. Faded-out and static poses are -64.
        if([BitConverter]::ToString($generated,23654,20) -cne [BitConverter]::ToString($original,23654,20) -or
           [BitConverter]::ToString($generated,23690,36) -cne [BitConverter]::ToString($original,23690,36)) { throw 'Sponsor bar timing or key1 changed.' }
        if((Read-BeFloat $generated 23674) -ne -64 -or (Read-BeFloat $generated 622126) -ne -64) { throw 'Sponsor bar faded-out/static pose is not hidden.' }
        $sponsorOn=Read-NativeValuePolynomial $original 23674 1
        if((Read-NativeValuePolynomial $generated 23674 1) -ne $sponsorOn) { throw 'Sponsor bar ON endpoint changed.' }
        $sponsorScale=(-64.0-$sponsorOn)/((Read-BeFloat $original 23674)-$sponsorOn);$previous=-64.0
        for($step=0;$step -le 1000;$step++) {
            [single]$u=$step/1000.0
            $native=Read-NativeValuePolynomial $original 23674 $u
            $actual=Read-NativeValuePolynomial $generated 23674 $u
            if([Math]::Abs($actual-($sponsorOn+$sponsorScale*($native-$sponsorOn))) -gt 0.0001 -or $actual -lt $previous-0.000002) { throw 'Sponsor bar travel changed its native curve shape or reverses direction.' }
            $previous=$actual
        }
        # Component-space bounds of every sponsor render vertex (bar, tier blocks
        # and caret line) at both anchor endpoints and all bottom-left parent extrema.
        foreach($cameraName in @('orthographic','perspective','headset-orthographic','headset-perspective','headset-2150x2284-perspective',
            'pc4090-orthographic','pc4090-perspective','pc5090-perspective')) {
            $camera=[IO.File]::ReadAllBytes((Join-Path $root "tests/fixtures/native-ui-$cameraName.bin"))
            foreach($parentY in @(-0.6074806451797485,0.440000057220459,3.832886219024658)) {
                foreach($anchorY in @(-0.7244457602500916,0.006857872009277344)) {
                    foreach($x in @(-3.618847370147705,13.859701273850847)) { foreach($y in @(-0.0001570284366607666,0.28824296593666077)) { foreach($z in @(0.0,0.006515756859538756)) {
                        $world=@(($x+0.4924473762512207-5.039999961853027),($y+(Read-BeFloat $generated 622126)+$anchorY+$parentY-3.240000009536743),
                            ($z-0.17761243879795074+3.80975535563266e-8),1.0)
                        $clip=@(0.0,0.0,0.0,0.0)
                        for($c=0;$c -lt 4;$c++) { for($r=0;$r -lt 4;$r++) { $clip[$c]+=$world[$r]*[BitConverter]::ToSingle($camera,64+16*$r+4*$c) } }
                        if($clip[3] -le 0 -or $clip[1] -ge -$clip[3]) { throw "Hidden sponsor bar intersects $cameraName canvas at parentY=$parentY." }
                    } } }
                }
            }
        }
        $sponsorReverted=[byte[]]$generated.Clone()
        Put-Hex $sponsorReverted 23674 $sponsorY[23674][0]
        Put-Hex $sponsorReverted 622126 $sponsorY[622018][0]
        if((Hash-Bytes $sponsorReverted) -ne $script:CinemaPopupHash) { throw 'Sponsor edit changed undeclared bytes.' }
        # Removing the new keys and reversing only the declared fields must
        # exactly recover the previously deployed asset, including every event.
        $stripped=[byte[]]$sponsorReverted.Clone()
        for($axis=2;$axis -ge 0;$axis--) {
            $offset=121516+146*$axis
            $next=[byte[]]::new($stripped.Length-36)
            [Array]::Copy($stripped,0,$next,0,$offset)
            [Array]::Copy($stripped,$offset+36,$next,$offset,$stripped.Length-$offset-36)
            $stripped=$next
        }
        foreach($offset in $popupEdits.Keys) { Put-Hex $stripped $offset $popupEdits[$offset][0] }
        if((Hash-Bytes $stripped) -ne $script:CinemaReplayOnlyHash) { throw 'Rebuild changed undeclared bytes.' }
        # Captured canonical, headset and PC4090/PC5090 camera MVPs, exact asset
        # transforms and the four popup-glass vertices. Include both parent
        # bar endpoints and its highest packaged menu position.
        $points=@(@(-40.3268928527832,0.14270348846912384,-6.096739980421262e-6),@(-40.3268928527832,-17.759849548339844,-7.0428659455501474e-6),
            @(40.326900482177734,-17.75984764099121,6.089192538638599e-6),@(40.326900482177734,0.14270348846912384,6.320063221210148e-6))
        foreach($cameraName in @('orthographic','perspective','headset-orthographic','headset-perspective',
            'pc4090-orthographic','pc4090-perspective','pc5090-perspective')) {
            $camera=[IO.File]::ReadAllBytes((Join-Path $root "tests/fixtures/native-ui-$cameraName.bin"))
            foreach($parentY in @(-0.6074806451797485,0.440000057220459,3.832886219024658)) {
                foreach($point in $points) {
                    $world=@((($point[0]+4.202795028686523)*1.1999999284744263+0.7487764358520508-5.039999961853027),
                        (($point[1]-0.027940109372138977)*1.1999999284744263+(Read-BeFloat $generated 121682)+$parentY-3.240000009536743),
                        (($point[2]-0.09208862483501434)*1.1999999284744263-0.18310017883777618+3.80975535563266e-8),1.0)
                    $clip=@(0.0,0.0,0.0,0.0)
                    for($c=0;$c -lt 4;$c++) { for($r=0;$r -lt 4;$r++) { $clip[$c]+=$world[$r]*[BitConverter]::ToSingle($camera,64+16*$r+4*$c) } }
                    if($clip[3] -le 0 -or $clip[1] -ge -$clip[3]) { throw "Hidden popup intersects $cameraName canvas at parentY=$parentY." }
                }
            }
        }
        Assert-CinemaHash $case.paths.target $script:CinemaOriginalHash
        Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Repeated prepare preserves source and backup'
        if($phase -in @('after-install','restore-temp')) {
            Install-CinemaMatte $case.game $case.run $entry
            Assert-CinemaHash $case.paths.target $script:CinemaModifiedHash
        }
        if($phase -eq 'install-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$modified) }
        if($phase -eq 'restore-temp') { [IO.File]::WriteAllBytes($case.paths.temporary,$original) }
        Restore-CinemaMatte $case.game $case.run $entry
        Restore-CinemaMatte $case.game $case.run $entry
        Assert-CinemaHash $case.paths.target $script:CinemaOriginalHash
        Assert-CinemaHash $case.paths.backup $script:CinemaOriginalHash
        if(Test-Path -LiteralPath $case.paths.temporary) { throw "Temporary file retained in $phase" }
    }
    foreach($legacy in @(@{name='index-only';bytes=$indexOnly;hash=$script:CinemaIndexOnlyHash},@{name='vertices-only';bytes=$verticesOnly;hash=$script:CinemaVerticesOnlyHash},@{name='replay-only';bytes=$replayOnly;hash=$script:CinemaReplayOnlyHash},@{name='popup';bytes=$popupOnly;hash=$script:CinemaPopupHash})) {
        $case=New-Case ('superseded-'+$legacy.name)
        [IO.File]::WriteAllBytes($case.paths.backup,$original)
        [IO.File]::WriteAllBytes($case.paths.source,$legacy.bytes)
        $entry=@{name='frontend/databases/overlayPersistent.pssg';originalSha256=$script:CinemaOriginalHash;sha256=$legacy.hash}
        Assert-Rejected { Install-CinemaMatte $case.game $case.run $entry } 'Superseded candidate cannot install'
        [IO.File]::WriteAllBytes($case.paths.target,$legacy.bytes)
        [IO.File]::WriteAllBytes($case.paths.temporary,$legacy.bytes)
        Restore-CinemaMatte $case.game $case.run $entry
        Restore-CinemaMatte $case.game $case.run $entry
        Assert-CinemaHash $case.paths.target $script:CinemaOriginalHash
        if(Test-Path -LiteralPath $case.paths.temporary) { throw 'Superseded temporary file retained.' }
    }
    foreach($changed in @('target','backup','temporary')) {
        $case=New-Case ('foreign-'+$changed)
        $entry=New-CinemaMatteDeployment $case.game $case.run
        Install-CinemaMatte $case.game $case.run $entry
        [IO.File]::WriteAllText($case.paths[$changed],'foreign or incomplete data')
        Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } "Foreign $changed"
    }
    $case=New-Case 'missing-backup'
    $entry=New-CinemaMatteDeployment $case.game $case.run
    Remove-Item -LiteralPath $case.paths.backup
    Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } 'Missing backup'
    $case=New-Case 'entry-guards'
    $entry=New-CinemaMatteDeployment $case.game $case.run
    $entry.name='../foreign.bin'
    Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } 'Escaping entry'
    $entry.name='frontend/databases/overlayPersistent.pssg'; $entry.sha256='0'*64
    Assert-Rejected { Restore-CinemaMatte $case.game $case.run $entry } 'Forged entry hash'
    foreach($changed in @('target','source','executable')) {
        $case=New-Case ('preinstall-'+$changed)
        $entry=New-CinemaMatteDeployment $case.game $case.run
        [IO.File]::WriteAllText($case.paths[$changed],'changed after preparation')
        Assert-Rejected { Install-CinemaMatte $case.game $case.run $entry } "Changed $changed before install"
    }
    foreach($owned in @('source','backup','temporary')) {
        $case=New-Case ('existing-'+$owned)
        [IO.File]::WriteAllText($case.paths[$owned],'foreign preexisting file')
        Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } "Existing $owned before prepare"
    }
    $case=New-Case 'wrong-index-payload'
    $bad=[byte[]]$original.Clone();$bad[456043]=7
    [IO.File]::WriteAllBytes($case.paths.target,$bad)
    $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
    try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong exact index payload' }
    finally { $script:CinemaOriginalHash=$saved }
    foreach($offset in @(514752,516161)) {
        $case=New-Case ('wrong-position-'+$offset)
        $bad=[byte[]]$original.Clone();$bad[$offset]=7
        [IO.File]::WriteAllBytes($case.paths.target,$bad)
        $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
        try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong exact position payload' }
        finally { $script:CinemaOriginalHash=$saved }
    }
    foreach($offset in @(120095,120131)) {
        $case=New-Case ('wrong-animation-'+$offset)
        $bad=[byte[]]$original.Clone();$bad[$offset]=7
        [IO.File]::WriteAllBytes($case.paths.target,$bad)
        $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
        try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong replay animation payload' }
        finally { $script:CinemaOriginalHash=$saved }
    }
    foreach($offset in $popupEdits.Keys) {
        $case=New-Case ('wrong-popup-field-'+$offset)
        $bad=[byte[]]$original.Clone();$bad[$offset]=$bad[$offset] -bxor 1
        [IO.File]::WriteAllBytes($case.paths.target,$bad)
        $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
        try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong popup field guard' }
        finally { $script:CinemaOriginalHash=$saved }
    }
    foreach($offset in @(23674,23689,622018)) {
        $case=New-Case ('wrong-sponsor-field-'+$offset)
        $bad=[byte[]]$original.Clone();$bad[$offset]=$bad[$offset] -bxor 1
        [IO.File]::WriteAllBytes($case.paths.target,$bad)
        $saved=$script:CinemaOriginalHash;$script:CinemaOriginalHash=Hash-Bytes $bad
        try { Assert-Rejected { New-CinemaMatteDeployment $case.game $case.run } 'Wrong sponsor bar field guard' }
        finally { $script:CinemaOriginalHash=$saved }
    }
    Write-Output 'Cinema matte, replay OFF, separate popup hidden frame and sponsor bar OFF: exact geometry/animation guards, unchanged ON endpoints, atomic replacement, interrupted recovery, legacy recovery and foreign-file refusal passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($fixture)
    $parent=[IO.Path]::GetFullPath((Join-Path $root 'artifacts'))
    if([IO.Path]::GetDirectoryName($resolved) -ne $parent -or [IO.Path]::GetFileName($resolved) -notmatch '^cinema-recovery-test-[0-9a-f]{32}$') { throw 'Fixture cleanup escaped artifacts.' }
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

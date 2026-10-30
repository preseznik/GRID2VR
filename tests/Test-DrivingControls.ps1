$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Driving-Controls.ps1')
$temp=Join-Path $root ('build/driving-controls-test-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
function Check($value,$message) { if(!$value) { throw $message } }
function Reject([string]$content) {
    [IO.File]::WriteAllText((Join-Path $temp 'input.xml'),$content)
    $rejected=$false
    try { $null=Read-DrivingControls (Join-Path $temp 'input.xml') } catch { $rejected=$true }
    Check $rejected 'Malformed controls accepted.'
}
try {
    $valid='<DrivingControls version="1"><ActionMap device_type="keyboard"><Action id="Accelerate"><Axis id="vk_code_0x57" type="uniDirPos" deadzone="0" saturation="1"/></Action></ActionMap><ActionMap device_type="xinput_pad"><Action id="Accelerate"><Axis id="xinput_right_trigger" type="uniDirPos" deadzone="0.1" saturation="0.9"/></Action></ActionMap></DrivingControls>'
    $source=Join-Path $temp 'input.xml';[IO.File]::WriteAllText($source,$valid)
    $controls=Read-DrivingControls $source
    Check ($controls.bindings -eq 2) 'Bindings did not validate.'
    Check (!$controls.clutchShiftSettings -and $controls.clutchShiftThreshold -eq 100) 'Legacy v1 unexpectedly enabled a relaxed clutch gate.'
    # Snapshot must use the validated bytes even if the source changes afterward.
    [IO.File]::WriteAllText($source,'changed')
    $receipt=Save-DrivingControls $controls $temp
    Check ((Get-FileHash (Join-Path $temp 'driving-controls.xml')).Hash -eq $receipt.sha256) 'Snapshot digest changed.'
    Check ([IO.File]::ReadAllText((Join-Path $temp 'driving-controls.xml')) -ceq $valid) 'Snapshot reread changed input.'
    $protocol=[IO.File]::ReadAllLines((Join-Path $temp 'driving-controls.request'))
    Check ($protocol.Count -eq 3 -and $protocol[1] -ceq "keyboard`tAccelerate`tvk_code_0x57`tuniDirPos`t0`t1") 'Native protocol is not canonical.'
    $family='{C262046D-0000-0000-0000-504944564944}'
    $first='{00000001-0002-0003-0004-000000000005}'
    $second='{00000002-0002-0003-0004-000000000005}'
    $axis='<Action id="Accelerate"><Axis id="di_y_axis" type="uniDirNeg" deadzone="0.02" saturation="1"/></Action>'
    $v2='<DrivingControls version="2"><ActionMap device_type="'+$family+'" device_id="'+$first+'">'+$axis+'</ActionMap><ActionMap device_type="'+$family+'" device_id="'+$second+'">'+$axis+'</ActionMap><ActionMap device_type="xinput_pad" device_id="xinput:0"><Action id="Accelerate"><Axis id="xinput_right_trigger" type="uniDirPos" deadzone="0" saturation="1"/></Action></ActionMap></DrivingControls>'
    [IO.File]::WriteAllText($source,$v2)
    $mixed=Read-DrivingControls $source
    Check ($mixed.bindings -eq 3) 'Same-family physical devices or simultaneous Xbox were lost.'
    Check (!$mixed.clutchShiftSettings -and $mixed.clutchShiftThreshold -eq 100) 'Legacy v2 unexpectedly enabled a relaxed clutch gate.'
    $v2Run=Join-Path $temp 'v2';New-Item -ItemType Directory -Path $v2Run | Out-Null
    $null=Save-DrivingControls $mixed $v2Run
    $protocol=[IO.File]::ReadAllLines((Join-Path $v2Run 'driving-controls.request'))
    Check ($protocol[0] -ceq 'GRID2VR_DRIVING_CONTROLS_2' -and $protocol.Count -eq 4) 'Wrong v2 protocol header/count.'
    Check ($protocol[1] -ceq "$family`tAccelerate`tdi_y_axis`tuniDirNeg`t0.02`t1`t$first") 'Physical identity/calibration lost in native snapshot.'
    foreach($mode in @('game','automatic','sequential','hpattern','hpattern_clutch')) {
        $v3=$v2.Replace('version="2"',('version="3" transmission="'+$mode+'" steering_linearity="0"'))
        [IO.File]::WriteAllText($source,$v3)
        $configured=Read-DrivingControls $source
        $modeRun=Join-Path $temp $mode;New-Item -ItemType Directory -Path $modeRun | Out-Null
        $modeReceipt=Save-DrivingControls $configured $modeRun
        $protocol=[IO.File]::ReadAllLines((Join-Path $modeRun 'driving-controls.request'))
        Check ($configured.transmission -ceq $mode -and $modeReceipt.transmission -ceq $mode) 'Transmission setting lost.'
        Check (!$configured.clutchShiftSettings -and !$modeReceipt.clutchShiftSettings -and $configured.clutchShiftThreshold -eq 100) 'Legacy v3 unexpectedly enabled a relaxed clutch gate.'
        Check ($protocol.Count -eq 5 -and $protocol[0] -ceq 'GRID2VR_DRIVING_CONTROLS_3' -and $protocol[1] -ceq "settings`t$mode`t0") 'Transmission protocol changed.'
        Check ($protocol[2] -ceq "$family`tAccelerate`tdi_y_axis`tuniDirNeg`t0.02`t1`t$first") 'v3 binding or calibration changed.'
    }
    foreach($mode in @('game','automatic','sequential','hpattern','hpattern_clutch')) {
        foreach($threshold in @(10,50,100)) {
            $v4=$v2.Replace('version="2"',('version="4" transmission="'+$mode+'" steering_linearity="-10" clutch_shift_threshold="'+$threshold+'"'))
            [IO.File]::WriteAllText($source,$v4)
            $configured=Read-DrivingControls $source
            $modeRun=Join-Path $temp ("v4-$mode-$threshold");New-Item -ItemType Directory -Path $modeRun | Out-Null
            $modeReceipt=Save-DrivingControls $configured $modeRun
            $protocol=[IO.File]::ReadAllLines((Join-Path $modeRun 'driving-controls.request'))
            Check ($configured.clutchShiftSettings -and $modeReceipt.clutchShiftSettings -and $configured.clutchShiftThreshold -eq $threshold -and $modeReceipt.clutchShiftThreshold -eq $threshold) 'Clutch shift setting lost.'
            Check ($protocol.Count -eq 5 -and $protocol[0] -ceq 'GRID2VR_DRIVING_CONTROLS_4' -and $protocol[1] -ceq "settings`t$mode`t-10`t$threshold") 'Clutch threshold protocol changed.'
            Check ($protocol[2] -ceq "$family`tAccelerate`tdi_y_axis`tuniDirNeg`t0.02`t1`t$first") 'Clutch threshold changed binding identity or analogue calibration.'
            Check ((Get-FileHash (Join-Path $modeRun 'driving-controls.xml')).Hash -eq $modeReceipt.sha256) 'v4 immutable snapshot digest changed.'
        }
    }
    $clutchOnly='<DrivingControls version="4" transmission="hpattern_clutch" steering_linearity="0" clutch_shift_threshold="50"/>'
    [IO.File]::WriteAllText($source,$clutchOnly)
    $thresholdOnly=Read-DrivingControls $source
    Check ($thresholdOnly.bindings -eq 0 -and $thresholdOnly.clutchShiftThreshold -eq 50 -and $thresholdOnly.clutchShiftSettings) 'v4 settings-only request refused.'
    $settingsRun=Join-Path $temp 'v4-settings-only';New-Item -ItemType Directory -Path $settingsRun | Out-Null
    $null=Save-DrivingControls $thresholdOnly $settingsRun
    $protocol=[IO.File]::ReadAllLines((Join-Path $settingsRun 'driving-controls.request'))
    Check ($protocol.Count -eq 2 -and $protocol[1] -ceq "settings`thpattern_clutch`t0`t50") 'Settings-only v4 protocol changed.'
    foreach($threshold in 10..100) {
        [IO.File]::WriteAllText($source,$clutchOnly.Replace('clutch_shift_threshold="50"',('clutch_shift_threshold="'+$threshold+'"')))
        Check ((Read-DrivingControls $source).clutchShiftThreshold -eq $threshold) 'Valid clutch percentage rejected.'
    }
    foreach($threshold in @('0','9','101','-10','+50','050','50.0','50.5',' 50','50 ','50&#10;','NaN','','999999999999999999999')) {
        Reject ($clutchOnly.Replace('clutch_shift_threshold="50"',('clutch_shift_threshold="'+$threshold+'"')))
    }
    Reject ($clutchOnly.Replace(' clutch_shift_threshold="50"',''))
    Reject ($clutchOnly.Replace('transmission="hpattern_clutch"','transmission="game"'))
    Reject ($clutchOnly.Replace('version="4"','version="3"'))
    Reject ($clutchOnly.Replace('version="4"','version="4" extra="1"'))
    foreach($mode in @('','HPattern','unknown','hpattern ')) { Reject ($v2.Replace('version="2"',('version="3" transmission="'+$mode+'"'))) }
    Reject ($v2.Replace('version="2"','version="3"'))
    Reject ($v2.Replace('version="2"','version="2" transmission="game"'))
    Reject ($v2.Replace('version="2"','version="3" transmission="game" extra="1"'))
    [IO.File]::WriteAllText($source,'<DrivingControls version="3" transmission="sequential" steering_linearity="0"/>')
    $transmissionOnly=Read-DrivingControls $source
    Check ($transmissionOnly.bindings -eq 0 -and $transmissionOnly.transmission -ceq 'sequential') 'Transmission-only configuration refused.'
    Reject '<DrivingControls version="3" transmission="game" steering_linearity="0"/>'
    foreach($value in -10..10) {
        [IO.File]::WriteAllText($source,('<DrivingControls version="3" transmission="sequential" steering_linearity="'+$value+'"/>'))
        Check ((Read-DrivingControls $source).steeringLinearity -eq $value) 'Valid steering response rejected.'
    }
    foreach($value in @('-11','11','-0','+1','01','1.0',' 1','NaN','')) {
        Reject ('<DrivingControls version="3" transmission="sequential" steering_linearity="'+$value+'"/>')
    }
    Reject ($v2.Replace($second,$first.ToLowerInvariant()))
    Reject ($v2.Replace('device_id="xinput:0"','device_id="xinput:4"'))
    Reject ($v2.Replace($first,'invalid'))
    Reject ($v2.Replace($first,' '+$first))
    Reject ($v2.Replace($family,$family+' '))
    Reject ($v2.Replace('deadzone="0.02" saturation="1"','deadzone="0.1000000000000000000000000001" saturation="0.1000000000000000000000000002"'))
    Check ($null -eq (Read-DrivingControls '')) 'Empty settings should preserve native controls.'
    Reject ($valid.Replace('version="1"','version="2"'))
    Reject ($valid.Replace('vk_code_0x57','vk_code_0x00'))
    Reject ($valid.Replace('xinput_right_trigger','di_button_0'))
    Reject ($valid.Replace('deadzone="0.1"','deadzone="0.9"'))
    Reject ($valid.Replace('deadzone="0.1"','deadzone="NaN"'))
    Reject ($valid.Replace('type="uniDirPos"','type="uniDirNeg"'))
    Reject ($valid.Replace('id="Accelerate"','id="Unknown"'))
    Reject ($valid.Replace('<Action id="Accelerate">','<Action id="Accelerate" extra="1">'))
    Reject ($valid.Replace('</ActionMap><ActionMap device_type="xinput_pad">','</ActionMap><ActionMap device_type="keyboard">'))
    Reject ($valid.Replace('keyboard','{invalid-guid}'))
    Reject '<!DOCTYPE DrivingControls [<!ENTITY x SYSTEM "file:///C:/Windows/win.ini">]><DrivingControls version="1">&x;</DrivingControls>'
    Reject '<DrivingControls version="1"/>'
    Reject ($valid.Replace('</Action>','text</Action>'))
    foreach($script in @('Start-UserSession.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
        $tokens=$null;$errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root ('tools/'+$script)),[ref]$tokens,[ref]$errors)
        Check (!$errors) "Invalid syntax in $script"
        Check (@($ast.ParamBlock.Parameters.Name.VariablePath.UserPath) -contains 'DrivingControls') "Missing argument in $script"
        Check (@($ast.ParamBlock.Parameters.Name.VariablePath.UserPath) -contains 'DeviceClaim') "Missing device claim argument in $script"
    }
    # Normal launches get the claim too: the native claim only fills a keyboard-only
    # profile map, so a device bound by Press Start or the Controls preset is kept.
    $start=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/Start-UserSession.ps1'),[ref]$null,[ref]$null)
    $forward=@($start.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -ceq '$parameters.DeviceClaim'},$true))
    Check ($forward.Count -eq 1 -and !($forward[0].Parent.Parent -is [Management.Automation.Language.IfStatementAst])) 'The device claim is not forwarded in every launch mode.'
    $diagnostic=Get-Content -LiteralPath (Join-Path $root 'tools/Run-Diagnostic.ps1') -Raw
    Check ($diagnostic.Contains('$deviceClaimWritten=[bool]($deviceClaimRequest -and ($SessionDirectory -or $NormalLaunch))')) 'Normal launches do not write the device claim request.'
    # Direct-launch device claim: exact native protocol, never for an invalid spec.
    Check ($null -eq (ConvertTo-DeviceClaimRequest '')) 'An empty claim spec produced a request.'
    Check ((ConvertTo-DeviceClaimRequest 'auto') -ceq "GRID2VR_DEVICE_CLAIM_1`nmode`tauto`n") 'Auto claim protocol is not canonical.'
    Check ((ConvertTo-DeviceClaimRequest "auto;$family;xinput_pad") -ceq "GRID2VR_DEVICE_CLAIM_1`nmode`tauto`nfamily`t$family`nfamily`txinput_pad`n") 'Steering families lost their order.'
    Check ((ConvertTo-DeviceClaimRequest 'device;xinput_pad') -ceq "GRID2VR_DEVICE_CLAIM_1`nmode`tdevice`nfamily`txinput_pad`n") 'Explicit device protocol is not canonical.'
    foreach($bad in @('off','keyboard','Auto','device','device;xinput_pad;'+$family,'auto;keyboard','auto;di_default','auto;','auto;xinput_pad;XINPUT_PAD',
        "auto;$family;$($family.ToLowerInvariant())","auto;xinput_pad`n",('auto'+(@(1..9 | ForEach-Object { ";{00000001-0002-0003-0004-00000000000$_}" }) -join '')))) {
        $rejected=$false;try { $null=ConvertTo-DeviceClaimRequest $bad } catch { $rejected=$true }
        Check $rejected "Invalid device claim accepted: $bad"
    }
    'Driving controls v1/v2/v3/v4 validation, clutch threshold, unchanged analogue calibration, transmission options, immutable snapshot, physical identity, native protocol, device claim protocol and worker parameters passed.'
} finally {
    $absolute=[IO.Path]::GetFullPath($temp)
    if (!$absolute.StartsWith([IO.Path]::GetFullPath((Join-Path $root 'build'))+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test cleanup path.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}

$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Device-Preset.ps1')
$temp=Join-Path $root ('build/device-preset-test-'+[Guid]::NewGuid().ToString('N'))
function Check($value,$message) { if(!$value) { throw $message } }
function Rejects([scriptblock]$Action,$message) { $rejected=$false;try { & $Action } catch { $rejected=$true };Check $rejected $message }
function Sha([string]$Text) { $sha=[Security.Cryptography.SHA256]::Create();try { [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($Text))).Replace('-','') } finally { $sha.Dispose() } }
try {
    $game=Join-Path $temp 'game';$run=Join-Path $temp 'run'
    New-Item -ItemType Directory -Path (Join-Path $game 'input'),$run | Out-Null
    # Synthetic install: the real anchor line plus one shipped wheel preset.
    $list="<?xml version=`"1.0`" encoding=`"utf-8`"?>`r`n<dataset>`r`n"+
        "  <xmlreader processor=`"Input`" filename=`"input/lg_g27.xml`"                      map=`"UPDATE`" pool=`"UPDATE_TEMPORARY`" userdata=`"presets`" />`r`n"+
        $script:DevicePresetAnchor+"`r`n  <xmlreader processor=`"InputStrings`" filename=`"input/strings.xml`" map=`"UPDATE`" pool=`"UPDATE_TEMPORARY`" userdata=`"strings`" />`r`n</dataset>`r`n"
    [IO.File]::WriteAllText((Join-Path $game 'input/paths.xml'),$list,[Text.Encoding]::ASCII)
    [IO.File]::WriteAllText((Join-Path $game 'input/lg_g27.xml'),"<ActionMap name=`"lg_g27`" device_type=`"{C29B046D-0000-0000-0000-504944564944}`" priority=`"0`"/>`r`n",[Text.Encoding]::ASCII)
    $script:DevicePresetPathsOriginalHash=Sha $list
    $at=$list.IndexOf($script:DevicePresetAnchor)+$script:DevicePresetAnchor.Length
    $script:DevicePresetPathsModifiedHash=Sha ($list.Substring(0,$at)+$script:DevicePresetLine+$list.Substring($at))

    $g920='{C262046D-0000-0000-0000-504944564944}';$g27='{C29B046D-0000-0000-0000-504944564944}';$id='{00000001-0002-0003-0004-000000000005}'
    $rows=@(@($g920,'Steer Left','di_x_axis','biDirLower','0','1',$id),@($g920,'Steer Right','di_x_axis','biDirUpper','0','1',$id),
        @($g920,'Accelerate','di_y_axis','uniDirNeg','0.02','1',$id),@($g920,'Handbrake','di_button_1','uniDirPos','0','1',$id),
        @($g920,'Menu Up','di_dpad_0_up','uniDirPos','0','1',$id),@($g920,'Clutch','di_slider_0','uniDirNeg','0','1',$id),
        @($g920,'Gear 1','di_button_12','uniDirPos','0','1',$id),@($g920,'Gear Reverse','di_button_11','uniDirPos','0','1',$id),
        @('xinput_pad','Steer Left','xinput_left_stick_x','biDirLower','0','1','xinput:0'),@('xinput_pad','Steer Right','xinput_left_stick_x','biDirUpper','0','1','xinput:0'))
    $controls=@{rows=$rows}
    $request="GRID2VR_DEVICE_CLAIM_1`nmode`tauto`nfamily`t$($g920.ToLowerInvariant())`nfamily`txinput_pad`n"

    Check ((Get-ShippedPresetFamilies $game).Contains($g27)) 'Shipped preset family was not found.'
    Check ($null -eq (New-DevicePresetDeployment $game $run '' $controls)) 'A preset was prepared without a claim.'
    Check ($null -eq (New-DevicePresetDeployment $game $run "GRID2VR_DEVICE_CLAIM_1`nmode`tauto`nfamily`txinput_pad`n" $controls)) 'An Xbox claim needs no preset.'
    Check ($null -eq (New-DevicePresetDeployment $game $run "GRID2VR_DEVICE_CLAIM_1`nmode`tdevice`nfamily`t$g27`n" @{rows=@(@($g27,'Steer Left','di_x_axis','biDirLower','0','1',$id),@($g27,'Steer Right','di_x_axis','biDirUpper','0','1',$id))})) 'A wheel with a shipped preset was replaced.'
    Check ($null -eq (New-DevicePresetDeployment $game $run $request @{rows=@($rows | Where-Object { $_[1] -notlike 'Steer *' })})) 'A preset without steering was prepared.'
    Check (@(Get-ChildItem -LiteralPath $run).Count -eq 0) 'Skipped preparation wrote run files.'

    $entry=New-DevicePresetDeployment $game $run $request $controls
    Check ($entry.family -ceq $g920 -and $entry.name -ceq 'input/paths.xml' -and $entry.presetName -ceq 'input/grid2vr_device.xml') 'Preset entry is incomplete.'
    $preset=[IO.File]::ReadAllText((Join-Path $run 'device-preset.xml'))
    Check ($preset.Contains("device_type=`"$g920`" priority=`"0`"") -and $preset.Contains('<Axis id="di_x_axis" type="biDirLower" deadzone="0" saturation="1" />')) 'Preset axes are not native.'
    Check ($preset.Contains('<Axis id="di_button_1" />') -and $preset.Contains('<Axis id="di_dpad_0_up" />')) 'Preset buttons are not native.'
    Check (!$preset.Contains('Clutch') -and !$preset.Contains('Gear 1') -and !$preset.Contains('Gear Reverse') -and !$preset.Contains('xinput')) 'Preset added clutch, H-pattern or another device.'
    $xml=[xml]$preset;Check ($xml.ActionMap.Action.Count -eq 5) 'Preset is not one well-formed action map.'
    $journal=$entry | ConvertTo-Json | ConvertFrom-Json

    $paths=Join-Path $game 'input/paths.xml';$installed=Join-Path $game 'input/grid2vr_device.xml'
    Install-DevicePreset $game $run $journal
    Check ((Get-FileHash $paths).Hash -eq $script:DevicePresetPathsModifiedHash -and (Get-FileHash $installed).Hash -eq $entry.presetSha256) 'Install did not deploy both files.'
    Check ((Get-ShippedPresetFamilies $game).Count -eq 1) 'The session preset counted as shipped.'
    Rejects { Install-DevicePreset $game $run $journal } 'A second install was accepted.'
    Restore-DevicePreset $game $run $journal
    Check ((Get-FileHash $paths).Hash -eq $script:DevicePresetPathsOriginalHash -and !(Test-Path -LiteralPath $installed)) 'Restore did not return the original installation.'
    Restore-DevicePreset $game $run $journal
    # Interrupted install: preset copied, list not yet replaced.
    Copy-Item -LiteralPath (Join-Path $run 'device-preset.xml') -Destination $installed
    Restore-DevicePreset $game $run $journal
    Check (!(Test-Path -LiteralPath $installed)) 'Partial install was not recovered.'

    Install-DevicePreset $game $run $journal
    [IO.File]::AppendAllText($installed,'<!-- edited -->')
    Rejects { Restore-DevicePreset $game $run $journal } 'A changed preset was removed.'
    Check ((Get-FileHash $paths).Hash -eq $script:DevicePresetPathsModifiedHash) 'Refused recovery changed the list.'
    Copy-Item -LiteralPath (Join-Path $run 'device-preset.xml') -Destination $installed -Force
    [IO.File]::AppendAllText($paths,' ')
    Rejects { Restore-DevicePreset $game $run $journal } 'A foreign preset list was replaced.'
    Copy-Item -LiteralPath (Join-Path $run 'device-preset-paths.xml') -Destination $paths -Force
    Restore-DevicePreset $game $run $journal
    Check ((Get-FileHash $paths).Hash -eq $script:DevicePresetPathsOriginalHash -and !(Test-Path -LiteralPath $installed)) 'Recovery after repair failed.'

    foreach($change in @(@{presetSha256='00'},@{name='input/other.xml'},@{family='xinput_pad'},@{sha256=$script:DevicePresetPathsOriginalHash})) {
        $tampered=$entry.Clone();foreach($key in $change.Keys){$tampered[$key]=$change[$key]}
        Rejects { Restore-DevicePreset $game $run $tampered } 'A tampered recovery entry was accepted.'
    }
    Rejects { Get-DevicePresetPaths $game (Join-Path $game 'input') } 'A backup inside the game was accepted.'
    $second=Join-Path $temp 'run2';New-Item -ItemType Directory -Path $second | Out-Null
    [IO.File]::AppendAllText($paths,' ')
    Rejects { New-DevicePresetDeployment $game $second $request $controls } 'An unknown preset list was modified.'
    'Device preset generation, skip rules, install, restore, partial recovery and tamper refusals passed.'
} finally {
    $absolute=[IO.Path]::GetFullPath($temp)
    if (!$absolute.StartsWith([IO.Path]::GetFullPath((Join-Path $root 'build'))+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test cleanup path.' }
    if(Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}

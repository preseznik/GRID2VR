$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/Recenter-Device.ps1')
function Encode($value) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($value | ConvertTo-Json -Compress))) }
function Reject([scriptblock]$action) { $failed=$false;try{& $action}catch{$failed=$true};if(!$failed){throw 'Invalid device recenter accepted.'} }
$binding=@{Action='Recenter';Device='Xbox';DeviceId='xinput:0';DeviceType='xinput_pad';Input='xinput_button_back'}
Assert-RecenterDevice $root (Encode $binding)
foreach($bad in @('xinput_left_trigger','xinput_button_guide','di_button_1')) { $copy=$binding.Clone();$copy.Input=$bad;Reject {Assert-RecenterDevice $root (Encode $copy)} }
$binding.DeviceId='{5a6ef250-7c39-4dbe-9314-c4f4415ca301}';$binding.DeviceType='{5a6ef250-7c39-4dbe-9314-c4f4415ca302}'
foreach($input in @('di_button_0','di_button_127','di_dpad_3_left')) { $binding.Input=$input;Assert-RecenterDevice $root (Encode $binding) }
foreach($input in @('di_button_128','di_dpad_4_up','di_x_axis')) { $binding.Input=$input;Reject {Assert-RecenterDevice $root (Encode $binding)} }
Reject {Assert-RecenterDevice $root ('A'*4097)}
foreach($script in @('Start-UserSession','Run-StereoProbe','Run-Diagnostic')) {
    $parameters=(Get-Command (Join-Path $root "tools/$script.ps1")).Parameters
    if(!$parameters.ContainsKey('RecenterDeviceBase64')) { throw "Missing binding route: $script" }
    if(!$parameters.ContainsKey('RecenterTilt') -or $parameters.RecenterTilt.ParameterType -ne [Management.Automation.SwitchParameter]) { throw "Missing recenter orientation route: $script" }
}
Write-Output 'Controller/wheel recenter validation and worker parameter routing passed; no device or game used.'

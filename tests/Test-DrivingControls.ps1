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
    # Snapshot must use the validated bytes even if the source changes afterward.
    [IO.File]::WriteAllText($source,'changed')
    $receipt=Save-DrivingControls $controls $temp
    Check ((Get-FileHash (Join-Path $temp 'driving-controls.xml')).Hash -eq $receipt.sha256) 'Snapshot digest changed.'
    Check ([IO.File]::ReadAllText((Join-Path $temp 'driving-controls.xml')) -ceq $valid) 'Snapshot reread changed input.'
    $protocol=[IO.File]::ReadAllLines((Join-Path $temp 'driving-controls.request'))
    Check ($protocol.Count -eq 3 -and $protocol[1] -ceq "keyboard`tAccelerate`tvk_code_0x57`tuniDirPos`t0`t1") 'Native protocol is not canonical.'
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
    }
    'Driving controls validation, immutable snapshot, native protocol and worker parameters passed.'
} finally {
    $absolute=[IO.Path]::GetFullPath($temp)
    if (!$absolute.StartsWith([IO.Path]::GetFullPath((Join-Path $root 'build'))+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test cleanup path.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}

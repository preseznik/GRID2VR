. (Join-Path $PSScriptRoot 'Paths.ps1')
# Read-only button polling runs in its own hidden launcher helper. No input injection.
function Assert-RecenterDevice([string]$Root,[string]$Encoded) {
    if(!$Encoded) { return }
    if($Encoded.Length -gt 4096) { throw 'Recenter binding exceeds its bound.' }
    $binding=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Encoded)) | ConvertFrom-Json
    if($binding.Action -cne 'Recenter' -or !$binding.Device -or $binding.Device.Length -gt 260) { throw 'Invalid recenter binding.' }
    if($binding.DeviceType -ceq 'xinput_pad') {
        $buttons=@('dpad_up','dpad_down','dpad_left','dpad_right','button_start','button_back','button_left_stick','button_right_stick','button_left_shoulder','button_right_shoulder','button_a','button_b','button_x','button_y') | ForEach-Object { 'xinput_'+$_ }
        if($binding.DeviceId -cnotmatch '^xinput:[0-3]$' -or $binding.Input -cnotin $buttons) { throw 'Invalid Xbox recenter button.' }
    } else {
        $id=[guid]::Empty; $product=[guid]::Empty
        if(![guid]::TryParseExact($binding.DeviceId,'B',[ref]$id) -or ![guid]::TryParseExact($binding.DeviceType,'B',[ref]$product) -or
           $binding.Input -cnotmatch '^di_(button_([0-9]|[1-9][0-9]|1[01][0-9]|12[0-7])|dpad_[0-3]_(up|down|left|right))$') { throw 'Invalid wheel recenter button.' }
    }
    if(!(Test-Path -LiteralPath (Get-Grid2Launcher $Root))) { throw 'Launcher is missing.' }
    if(Test-Grid2Package $Root) {
        $null=Get-Grid2PackageFile $Root 'payload/driving_input.dll'
    } else {
        foreach($name in @('GRID2VR.dll','driving_input.dll')) {
            if(!(Test-Path -LiteralPath (Join-Path $Root "build/launcher/$name"))) { throw "Recenter helper dependency is missing: $name" }
        }
    }
}

function Start-RecenterDevice([string]$Root,[string]$Run,[string]$Encoded,[Diagnostics.Process]$GameProcess) {
    Assert-RecenterDevice $Root $Encoded
    $info=[Diagnostics.ProcessStartInfo]::new((Get-Grid2Launcher $Root))
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WorkingDirectory=$Root
    foreach($arg in @('--watch-recenter',$Run,$Encoded,[string]$GameProcess.Id,[string]$GameProcess.StartTime.ToUniversalTime().Ticks)) { $info.ArgumentList.Add($arg) }
    $helper=[Diagnostics.Process]::Start($info)
    if(!$helper) { throw 'Could not start device recenter helper.' }
    $helper.Dispose()
}

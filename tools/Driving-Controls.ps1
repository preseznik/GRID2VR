# Session-only binding input. No game/profile files are changed by this helper.
function Read-DrivingControls([string]$Path) {
    if (!$Path) { return $null }
    $item=Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.Length -lt 1 -or $item.Length -gt 524288) { throw 'Invalid driving controls file.' }
    $bytes=[IO.File]::ReadAllBytes($item.FullName)
    if ($bytes.Length -gt 524288) { throw 'Driving controls file exceeds the size limit.' }
    $settings=[Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver=$null
    $settings.MaxCharactersInDocument=524288
    $stream=[IO.MemoryStream]::new($bytes,$false)
    $reader=[Xml.XmlReader]::Create($stream,$settings)
    $xml=[Xml.XmlDocument]::new();$xml.XmlResolver=$null
    try { $xml.Load($reader) } finally { $reader.Dispose();$stream.Dispose() }
    function Assert-Attributes($node,[string[]]$names) {
        if ($node.NamespaceURI -or $node.Attributes.Count -ne $names.Count) { throw 'Unexpected driving controls attributes.' }
        foreach($name in $names) { if (!$node.HasAttribute($name)) { throw 'Missing driving controls attribute.' } }
    }
    function Get-Elements($node,[string]$name) {
        $result=@()
        foreach($child in $node.ChildNodes) {
            if ($child.NodeType -in @([Xml.XmlNodeType]::Whitespace,[Xml.XmlNodeType]::SignificantWhitespace)) { continue }
            if ($child.NodeType -ne [Xml.XmlNodeType]::Element -or $child.Name -cne $name) { throw 'Unexpected driving controls element.' }
            $result+=,$child
        }
        return $result
    }
    $root=$xml.DocumentElement
    if (!$root -or $root.Name -cne 'DrivingControls' -or $root.GetAttribute('version') -cnotin @('1','2','3','4')) { throw 'Unsupported driving controls version.' }
    $version=[int]$root.GetAttribute('version')
    $transmission='game'
    $linearity=0
    $clutchShiftThreshold=100
    if ($version -ge 3) {
        $attributes=@('version','transmission','steering_linearity')
        if ($version -eq 4) { $attributes+=,'clutch_shift_threshold' }
        Assert-Attributes $root $attributes
        $transmission=$root.GetAttribute('transmission')
        if ($transmission -cnotin @('game','automatic','sequential','hpattern','hpattern_clutch')) { throw 'Invalid driving transmission mode.' }
        $text=$root.GetAttribute('steering_linearity')
        if ($text -cnotmatch '^(0|-?[1-9]|-?10)$' -or ![int]::TryParse($text,[ref]$linearity)) { throw 'Invalid steering linearity.' }
        if ($version -eq 4) {
            $text=$root.GetAttribute('clutch_shift_threshold')
            if ($text -cnotmatch '\A([1-9][0-9]|100)\z' -or ![int]::TryParse($text,[ref]$clutchShiftThreshold)) { throw 'Clutch shift threshold must be a whole percentage between 10 and 100.' }
        }
    } else { Assert-Attributes $root @('version') }
    $actions=@('Steer Left','Steer Right','Accelerate','Brake','Clutch','Handbrake','Gear Up','Gear Down','Gear 1','Gear 2','Gear 3','Gear 4','Gear 5','Gear 6','Gear 7','Gear Reverse','Change View','Look Back','Look Left','Look Right','Look Up','Look Down','Horn','Boost','Pause','Instant Replay','Menu Start Button','Menu Select','Menu Back','Menu Up','Menu Down','Menu Left','Menu Right','Replay Rewind','Replay Fast Forward','Replay Pause','Replay Next Camera','Replay Prev Camera','Replay Jump In','Replay Exit','Replay UI On Off')
    $xboxAxes=@('xinput_left_stick_x','xinput_left_stick_y','xinput_right_stick_x','xinput_right_stick_y','xinput_left_trigger','xinput_right_trigger')
    $xboxButtons=@('xinput_dpad_up','xinput_dpad_down','xinput_dpad_left','xinput_dpad_right','xinput_button_start','xinput_button_back','xinput_button_left_stick','xinput_button_right_stick','xinput_button_left_shoulder','xinput_button_right_shoulder','xinput_button_a','xinput_button_b','xinput_button_x','xinput_button_y')
    $directAxes=@('di_x_axis','di_y_axis','di_z_axis','di_x_axis_rotation','di_y_axis_rotation','di_z_axis_rotation','di_slider_0','di_slider_1')
    $slots=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $devices=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $rows=@();$steering=@{}
    foreach($map in @(Get-Elements $root 'ActionMap')) {
        if ($version -ge 2) { Assert-Attributes $map @('device_type','device_id') } else { Assert-Attributes $map @('device_type') }
        $device=$map.GetAttribute('device_type');$guid=[Guid]::Empty
        if ($device -cnotin @('keyboard','xinput_pad') -and ($device.Length -ne 38 -or ![Guid]::TryParseExact($device,'B',[ref]$guid))) { throw 'Invalid driving controls device family.' }
        $deviceId=if($version -ge 2){$map.GetAttribute('device_id')}else{''}
        if ($version -ge 2) {
            $validId=if($device -ceq 'keyboard'){$deviceId -ceq 'keyboard'}elseif($device -ceq 'xinput_pad'){$deviceId -cmatch '^xinput:[0-3]$'}else{$deviceId.Length -eq 38 -and [Guid]::TryParseExact($deviceId,'B',[ref]$guid)}
            if (!$validId) { throw 'Invalid driving controls device identity.' }
        }
        $identity=if($version -ge 2){$deviceId}else{$device}
        if (!$devices.Add($identity) -or $devices.Count -gt 16) { throw 'Duplicate or excessive driving controls devices.' }
        $mapActions=@(Get-Elements $map 'Action')
        if (!$mapActions.Count) { throw 'Empty driving controls map.' }
        foreach($action in $mapActions) {
            Assert-Attributes $action @('id');$id=$action.GetAttribute('id')
            if ($id -cnotin $actions -or !$slots.Add("$identity/$id")) { throw 'Invalid or duplicate driving action.' }
            $axes=@(Get-Elements $action 'Axis')
            if ($axes.Count -ne 1) { throw 'Each driving action requires one axis.' }
            $axis=$axes[0];Assert-Attributes $axis @('id','type','deadzone','saturation')
            if (@(Get-Elements $axis 'none').Count) { throw 'Nested driving axis content.' }
            $input=$axis.GetAttribute('id');$type=$axis.GetAttribute('type')
            $validInput=if($device -ceq 'keyboard') { $input -cmatch '^vk_code_0x[0-9A-F]{2}$' -and [Convert]::ToInt32($input.Substring(10),16) -ge 8 -and [Convert]::ToInt32($input.Substring(10),16) -le 254 }
                elseif($device -ceq 'xinput_pad') { $input -cin $xboxAxes -or $input -cin $xboxButtons }
                else { $input -cin $directAxes -or $input -cmatch '^di_dpad_[0-3]_(up|down|left|right)$' -or $input -cmatch '^di_button_([0-9]|[1-9][0-9]|1[01][0-9]|12[0-7])$' }
            $dead=[decimal]0;$sat=[decimal]0
            if (!$validInput -or $type -cnotin @('uniDirPos','uniDirNeg','biDirLower','biDirUpper') -or
                ![decimal]::TryParse($axis.GetAttribute('deadzone'),[Globalization.NumberStyles]::AllowDecimalPoint,[Globalization.CultureInfo]::InvariantCulture,[ref]$dead) -or
                ![decimal]::TryParse($axis.GetAttribute('saturation'),[Globalization.NumberStyles]::AllowDecimalPoint,[Globalization.CultureInfo]::InvariantCulture,[ref]$sat) -or
                $dead -lt 0 -or $dead -gt .9 -or $sat -lt .1 -or $sat -gt 1 -or $dead -ge $sat -or [float]$dead -ge [float]$sat) { throw 'Invalid driving axis or calibration.' }
            if ($device -ceq 'xinput_pad' -and $input.EndsWith('_trigger') -and $type -cne 'uniDirPos') { throw 'Xbox triggers require positive calibration.' }
            if ($id -cin @('Steer Left','Steer Right')) {
                $signature="$device/$input/$type"
                $other=if($id -ceq 'Steer Left'){'Steer Right'}else{'Steer Left'}
                if ($steering["$identity/$other"] -ceq $signature) { throw 'Left and right steering use the same direction.' }
                $steering["$identity/$id"]=$signature
            }
            $row=@($device,$id,$input,$type,$dead.ToString([Globalization.CultureInfo]::InvariantCulture),$sat.ToString([Globalization.CultureInfo]::InvariantCulture))
            if ($version -ge 2) { $row+=,$deviceId }
            $rows+= ,$row
        }
    }
    if (($rows.Count -lt 1 -and ($version -lt 3 -or $transmission -ceq 'game')) -or $rows.Count -gt $actions.Count*16) { throw 'Invalid driving controls count.' }
    $sha=[Security.Cryptography.SHA256]::Create()
    try { $hash=[BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','') } finally { $sha.Dispose() }
    return @{bytes=$bytes;sha256=$hash;bindings=$rows.Count;rows=$rows;version=$version;transmission=$transmission;steeringLinearity=$linearity;clutchShiftThreshold=$clutchShiftThreshold;clutchShiftSettings=($version -eq 4)}
}

function Save-DrivingControls($Controls,[string]$Run) {
    if (!$Controls) { return $null }
    $target=Join-Path $Run 'driving-controls.xml'
    if (Test-Path -LiteralPath $target) { throw 'Driving controls snapshot already exists.' }
    [IO.File]::WriteAllBytes($target,$Controls.bytes)
    # A bounded, validated ASCII protocol avoids exposing the native hook to an XML parser.
    $lines=@("GRID2VR_DRIVING_CONTROLS_$($Controls.version)")
    if ($Controls.version -ge 3) {
        $configuration="settings`t$($Controls.transmission)`t$($Controls.steeringLinearity)"
        if ($Controls.version -eq 4) { $configuration+="`t$($Controls.clutchShiftThreshold)" }
        $lines+=,$configuration
    }
    $lines+=@($Controls.rows | ForEach-Object { $_ -join "`t" })
    [IO.File]::WriteAllText((Join-Path $Run 'driving-controls.request'),($lines -join "`n")+"`n",[Text.Encoding]::ASCII)
    return @{sha256=$Controls.sha256;bindings=$Controls.bindings;transmission=$Controls.transmission;steeringLinearity=$Controls.steeringLinearity;clutchShiftThreshold=$Controls.clutchShiftThreshold;clutchShiftSettings=$Controls.clutchShiftSettings;sessionOnly=$true}
}
# Direct Practice/Race skip GRID 2's Press Start device claim. The launcher
# spec is 'auto[;family...]' (steering devices first; native adds xinput_pad)
# or 'device;family'. Families are xinput_pad or a DirectInput product GUID.
function ConvertTo-DeviceClaimRequest([string]$Spec) {
    if (!$Spec) { return $null }
    $parts=$Spec.Split(';');$mode=$parts[0];$families=@($parts | Select-Object -Skip 1)
    if ($mode -cnotin @('auto','device') -or $families.Count -gt 8 -or ($mode -ceq 'device' -and $families.Count -ne 1)) { throw 'Invalid direct-launch device claim.' }
    $seen=@{}
    foreach($family in $families) {
        if ($family -cnotmatch '\A(xinput_pad|\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\})\z') { throw 'Invalid direct-launch device family.' }
        if ($seen.ContainsKey($family.ToLowerInvariant())) { throw 'Duplicate direct-launch device family.' }
        $seen[$family.ToLowerInvariant()]=$true
    }
    return ((@('GRID2VR_DEVICE_CLAIM_1',"mode`t$mode")+@($families | ForEach-Object { "family`t$_" })) -join "`n")+"`n"
}

# Session-only binding input. No game/profile files are changed by this helper.
function Read-DrivingControls([string]$Path) {
    if (!$Path) { return $null }
    $item=Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.Length -lt 1 -or $item.Length -gt 131072) { throw 'Invalid driving controls file.' }
    $bytes=[IO.File]::ReadAllBytes($item.FullName)
    if ($bytes.Length -gt 131072) { throw 'Driving controls file exceeds the size limit.' }
    $settings=[Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit
    $settings.XmlResolver=$null
    $settings.MaxCharactersInDocument=131072
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
    if (!$root -or $root.Name -cne 'DrivingControls' -or $root.GetAttribute('version') -cne '1') { throw 'Unsupported driving controls version.' }
    Assert-Attributes $root @('version')
    $actions=@('Steer Left','Steer Right','Accelerate','Brake','Clutch','Handbrake','Gear Up','Gear Down','Gear 1','Gear 2','Gear 3','Gear 4','Gear 5','Gear 6','Gear 7','Gear Reverse','Change View','Look Back','Look Left','Look Right','Look Up','Look Down','Horn','Boost','Pause','Instant Replay','Menu Start Button','Menu Select','Menu Back','Menu Up','Menu Down','Menu Left','Menu Right','Replay Rewind','Replay Fast Forward','Replay Pause','Replay Next Camera','Replay Prev Camera','Replay Jump In','Replay Exit','Replay UI On Off')
    $xboxAxes=@('xinput_left_stick_x','xinput_left_stick_y','xinput_right_stick_x','xinput_right_stick_y','xinput_left_trigger','xinput_right_trigger')
    $xboxButtons=@('xinput_dpad_up','xinput_dpad_down','xinput_dpad_left','xinput_dpad_right','xinput_button_start','xinput_button_back','xinput_button_left_stick','xinput_button_right_stick','xinput_button_left_shoulder','xinput_button_right_shoulder','xinput_button_a','xinput_button_b','xinput_button_x','xinput_button_y')
    $directAxes=@('di_x_axis','di_y_axis','di_z_axis','di_x_axis_rotation','di_y_axis_rotation','di_z_axis_rotation','di_slider_0','di_slider_1')
    $slots=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $devices=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $rows=@();$steering=@{}
    foreach($map in @(Get-Elements $root 'ActionMap')) {
        Assert-Attributes $map @('device_type')
        $device=$map.GetAttribute('device_type');$guid=[Guid]::Empty
        if ($device -cnotin @('keyboard','xinput_pad') -and ![Guid]::TryParseExact($device,'B',[ref]$guid)) { throw 'Invalid driving controls device family.' }
        if (!$devices.Add($device)) { throw 'Duplicate driving controls device family.' }
        $mapActions=@(Get-Elements $map 'Action')
        if (!$mapActions.Count) { throw 'Empty driving controls map.' }
        foreach($action in $mapActions) {
            Assert-Attributes $action @('id');$id=$action.GetAttribute('id')
            if ($id -cnotin $actions -or !$slots.Add("$($device -ceq 'keyboard')/$id")) { throw 'Invalid or duplicate driving action.' }
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
                $dead -lt 0 -or $dead -gt .9 -or $sat -lt .1 -or $sat -gt 1 -or $dead -ge $sat) { throw 'Invalid driving axis or calibration.' }
            if ($device -ceq 'xinput_pad' -and $input.EndsWith('_trigger') -and $type -cne 'uniDirPos') { throw 'Xbox triggers require positive calibration.' }
            if ($id -cin @('Steer Left','Steer Right')) {
                $signature="$device/$input/$type"
                $other=if($id -ceq 'Steer Left'){'Steer Right'}else{'Steer Left'}
                if ($steering["$($device -ceq 'keyboard')/$other"] -ceq $signature) { throw 'Left and right steering use the same direction.' }
                $steering["$($device -ceq 'keyboard')/$id"]=$signature
            }
            $rows+= ,@($device,$id,$input,$type,$dead.ToString([Globalization.CultureInfo]::InvariantCulture),$sat.ToString([Globalization.CultureInfo]::InvariantCulture))
        }
    }
    if ($rows.Count -lt 1 -or $rows.Count -gt $actions.Count*2) { throw 'Invalid driving controls count.' }
    $sha=[Security.Cryptography.SHA256]::Create()
    try { $hash=[BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','') } finally { $sha.Dispose() }
    return @{bytes=$bytes;sha256=$hash;bindings=$rows.Count;rows=$rows}
}

function Save-DrivingControls($Controls,[string]$Run) {
    if (!$Controls) { return $null }
    $target=Join-Path $Run 'driving-controls.xml'
    if (Test-Path -LiteralPath $target) { throw 'Driving controls snapshot already exists.' }
    [IO.File]::WriteAllBytes($target,$Controls.bytes)
    # A bounded, validated ASCII protocol avoids exposing the native hook to an XML parser.
    $lines=@('GRID2VR_DRIVING_CONTROLS_1')+@($Controls.rows | ForEach-Object { $_ -join "`t" })
    [IO.File]::WriteAllText((Join-Path $Run 'driving-controls.request'),($lines -join "`n")+"`n",[Text.Encoding]::ASCII)
    return @{sha256=$Controls.sha256;bindings=$Controls.bindings;sessionOnly=$true}
}

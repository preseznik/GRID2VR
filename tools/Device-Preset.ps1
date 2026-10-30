# A session claims the driving device the way GRID 2's Press Start would.
# A DirectInput wheel newer than the game (e.g. Logitech G920) has no shipped
# preset, so the native bind finds nothing and FFB stays off. For the session
# only, add one preset built from the launcher's own bindings for that wheel
# and list it in input/paths.xml. Both files are restored after the game exits.
$script:DevicePresetPathsOriginalHash='D646F584FFB9783C4F7184AAC64E7DE3361867DADE7EEACAC9706B7A4C764369'
$script:DevicePresetPathsModifiedHash='836860A4FC0BC66FE5A6A9966ABF1A4C3D293863501B4EE463BB8D03ED77F5B9'
# Appended after the last shipped preset so in-game preset indices are unchanged.
$script:DevicePresetAnchor='  <xmlreader processor="Input" filename="input/xinput_speed_wheel.xml"          map="UPDATE" pool="UPDATE_TEMPORARY" userdata="presets" />'+"`r`n"
$script:DevicePresetLine='  <xmlreader processor="Input" filename="input/grid2vr_device.xml" map="UPDATE" pool="UPDATE_TEMPORARY" userdata="presets" />'+"`r`n"
# GRID2VR supplies clutch and H-pattern outputs when the native map lacks them;
# a session preset must not add native carriers for them.
$script:DevicePresetExcluded=@('Clutch','Gear 1','Gear 2','Gear 3','Gear 4','Gear 5','Gear 6','Gear 7','Gear Reverse')

function Assert-DevicePresetPlainPath([string]$Path) {
    $current=[IO.Path]::GetFullPath($Path)
    while($current) {
        if((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Linked device preset path retained: $current" }
        $current=Split-Path $current -Parent
    }
}

function Assert-DevicePresetHash([string]$Path,[string]$Expected) {
    Assert-DevicePresetPlainPath $Path
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-FileHash -LiteralPath $Path).Hash -ne $Expected) { throw "Changed or missing device preset file retained: $Path" }
}

function Get-DevicePresetPaths([string]$Game,[string]$Run) {
    $gamePath=[IO.Path]::GetFullPath($Game).TrimEnd('\')
    $runPath=[IO.Path]::GetFullPath($Run).TrimEnd('\')
    if($runPath -eq $gamePath -or $runPath.StartsWith($gamePath+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Device preset backup must be outside the installation.' }
    if(!(Test-Path -LiteralPath (Join-Path $gamePath 'input') -PathType Container) -or !(Test-Path -LiteralPath $runPath -PathType Container)) { throw 'Device preset game/run directory is missing.' }
    $paths=@{
        target=(Join-Path $gamePath 'input\paths.xml')
        preset=(Join-Path $gamePath 'input\grid2vr_device.xml')
        temporary=(Join-Path $gamePath 'input\GRID2VR.paths.tmp')
        source=(Join-Path $runPath 'device-preset-paths.xml')
        backup=(Join-Path $runPath 'device-preset-paths-original.xml')
        generated=(Join-Path $runPath 'device-preset.xml')
    }
    foreach($path in $paths.Values) { Assert-DevicePresetPlainPath $path }
    return $paths
}

function Read-DevicePresetXml([string]$Path) {
    $settings=[Xml.XmlReaderSettings]::new()
    $settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null;$settings.MaxCharactersInDocument=1048576
    $reader=[Xml.XmlReader]::Create($Path,$settings)
    try { $xml=[Xml.XmlDocument]::new();$xml.XmlResolver=$null;$xml.Load($reader);return $xml } finally { $reader.Dispose() }
}

# Device families that already have a shipped preset listed in paths.xml.
function Get-ShippedPresetFamilies([string]$Game) {
    $folder=Join-Path ([IO.Path]::GetFullPath($Game)) 'input'
    $families=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($reader in (Read-DevicePresetXml (Join-Path $folder 'paths.xml')).SelectNodes('/dataset/xmlreader[@userdata="presets"]')) {
        $name=$reader.GetAttribute('filename')
        if($name -cnotmatch '\Ainput/[a-z0-9_]+\.xml\z' -or $name -ceq 'input/grid2vr_device.xml') { continue }
        $file=Join-Path $folder $name.Substring(6)
        if(!(Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        foreach($attribute in (Read-DevicePresetXml $file).DocumentElement.Attributes) {
            if($attribute.Name -cmatch '\Adevice_type(_[0-9]+)?\z') { $null=$families.Add($attribute.Value) }
        }
    }
    return ,$families
}

# One native ActionMap from the validated launcher rows of one device family.
function New-DevicePresetText($Controls,[string]$Family) {
    $actions=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $lines=@("<!-- GRID2VR session preset for $Family, generated from launcher bindings; removed after the session. -->",
        "<ActionMap name=`"grid2vr_device`" device_type=`"$Family`" priority=`"0`">")
    foreach($row in $Controls.rows) {
        if(!$row[0].Equals($Family,[StringComparison]::OrdinalIgnoreCase) -or $row[1] -cin $script:DevicePresetExcluded -or !$actions.Add($row[1])) { continue }
        $axis=if($row[2] -cmatch '\Adi_(button|dpad)_'){"<Axis id=`"$($row[2])`" />"}else{"<Axis id=`"$($row[2])`" type=`"$($row[3])`" deadzone=`"$($row[4])`" saturation=`"$($row[5])`" />"}
        $lines+=@("  <Action id=`"$($row[1])`">","    $axis",'  </Action>')
    }
    if(!$actions.Contains('Steer Left') -or !$actions.Contains('Steer Right')) { return $null }
    return (($lines+'</ActionMap>') -join "`r`n")+"`r`n"
}

function Assert-DevicePresetEntry($Entry) {
    if(!$Entry -or $Entry.name -cne 'input/paths.xml' -or $Entry.originalSha256 -ne $script:DevicePresetPathsOriginalHash -or
       $Entry.sha256 -ne $script:DevicePresetPathsModifiedHash -or $Entry.presetName -cne 'input/grid2vr_device.xml' -or
       $Entry.presetSha256 -cnotmatch '\A[0-9A-F]{64}\z' -or $Entry.family -cnotmatch '\A\{[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}\}\z') {
        throw 'Unsupported device preset recovery entry.'
    }
}

# Returns $null when no claimed DirectInput family lacks a shipped preset.
function New-DevicePresetDeployment([string]$Game,[string]$Run,[string]$ClaimRequest,$Controls) {
    if(!$ClaimRequest -or !$Controls) { return $null }
    $shipped=Get-ShippedPresetFamilies $Game
    $family=$null;$text=$null
    foreach($line in $ClaimRequest.Split("`n")) {
        if($line -cnotmatch '\Afamily\t(\{[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}\})\z') { continue }
        $candidate=$Matches[1].ToUpperInvariant()
        if($shipped.Contains($candidate)) { continue }
        $text=New-DevicePresetText $Controls $candidate
        if($text) { $family=$candidate;break }
    }
    if(!$family) { return $null }
    $paths=Get-DevicePresetPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Close GRID 2 before preparing the device preset.' }
    Assert-DevicePresetHash $paths.target $script:DevicePresetPathsOriginalHash
    foreach($path in @($paths.preset,$paths.temporary,$paths.source,$paths.backup,$paths.generated)) {
        if(Test-Path -LiteralPath $path) { throw "Existing device preset file retained: $path" }
    }
    [IO.File]::WriteAllText($paths.generated,$text,[Text.Encoding]::ASCII)
    $original=[IO.File]::ReadAllText($paths.target,[Text.Encoding]::ASCII)
    $at=$original.IndexOf($script:DevicePresetAnchor,[StringComparison]::Ordinal)
    if($at -lt 0 -or $original.IndexOf($script:DevicePresetAnchor,$at+1,[StringComparison]::Ordinal) -ge 0) { throw 'Unexpected preset list layout.' }
    $at+=$script:DevicePresetAnchor.Length
    [IO.File]::WriteAllText($paths.source,$original.Substring(0,$at)+$script:DevicePresetLine+$original.Substring($at),[Text.Encoding]::ASCII)
    Assert-DevicePresetHash $paths.source $script:DevicePresetPathsModifiedHash
    [IO.File]::Copy($paths.target,$paths.backup,$false)
    Assert-DevicePresetHash $paths.backup $script:DevicePresetPathsOriginalHash
    return @{name='input/paths.xml';originalSha256=$script:DevicePresetPathsOriginalHash;sha256=$script:DevicePresetPathsModifiedHash;
        presetName='input/grid2vr_device.xml';presetSha256=(Get-FileHash -LiteralPath $paths.generated).Hash;family=$family}
}

function Write-DevicePresetPathsAtomic($Paths,[string]$Source,[string]$Before,[string]$After) {
    [IO.File]::Copy($Source,$Paths.temporary,$false)
    Assert-DevicePresetHash $Paths.temporary $After
    $file=[IO.File]::Open($Paths.temporary,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $file.Flush($true) } finally { $file.Dispose() }
    Assert-DevicePresetHash $Paths.target $Before
    [IO.File]::Replace($Paths.temporary,$Paths.target,[Management.Automation.Language.NullString]::Value)
    Assert-DevicePresetHash $Paths.target $After
}

function Install-DevicePreset([string]$Game,[string]$Run,$Entry) {
    Assert-DevicePresetEntry $Entry
    $paths=Get-DevicePresetPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'Existing GRID 2 process retained.' }
    Assert-DevicePresetHash $paths.target $Entry.originalSha256
    Assert-DevicePresetHash $paths.backup $Entry.originalSha256
    Assert-DevicePresetHash $paths.source $Entry.sha256
    Assert-DevicePresetHash $paths.generated $Entry.presetSha256
    if((Test-Path -LiteralPath $paths.preset) -or (Test-Path -LiteralPath $paths.temporary)) { throw 'Existing device preset file retained.' }
    # The preset exists before the list names it.
    [IO.File]::Copy($paths.generated,$paths.preset,$false)
    Assert-DevicePresetHash $paths.preset $Entry.presetSha256
    Write-DevicePresetPathsAtomic $paths $paths.source $Entry.originalSha256 $Entry.sha256
}

function Restore-DevicePreset([string]$Game,[string]$Run,$Entry) {
    Assert-DevicePresetEntry $Entry
    $paths=Get-DevicePresetPaths $Game $Run
    if(Get-Process grid2,grid2_avx -ErrorAction SilentlyContinue) { throw 'GRID 2 must exit before restoring the device preset.' }
    Assert-DevicePresetHash $paths.backup $Entry.originalSha256
    if(!(Test-Path -LiteralPath $paths.target -PathType Leaf)) { throw 'Missing preset list retained for inspection.' }
    $current=(Get-FileHash -LiteralPath $paths.target).Hash
    if($current -notin @($Entry.originalSha256,$Entry.sha256)) { throw 'Changed preset list retained; recovery requires inspection.' }
    if(Test-Path -LiteralPath $paths.preset) {
        if((Get-FileHash -LiteralPath $paths.preset).Hash -ne $Entry.presetSha256) { throw 'Changed device preset retained for inspection.' }
    }
    if(Test-Path -LiteralPath $paths.temporary) {
        if((Get-FileHash -LiteralPath $paths.temporary).Hash -notin @($Entry.originalSha256,$Entry.sha256)) { throw 'Foreign preset list temporary file retained.' }
        Remove-Item -LiteralPath $paths.temporary
    }
    # The list stops naming the preset before the preset is removed.
    if($current -eq $Entry.sha256) { Write-DevicePresetPathsAtomic $paths $paths.backup $Entry.sha256 $Entry.originalSha256 }
    Assert-DevicePresetHash $paths.target $Entry.originalSha256
    if(Test-Path -LiteralPath $paths.preset) { Remove-Item -LiteralPath $paths.preset }
}

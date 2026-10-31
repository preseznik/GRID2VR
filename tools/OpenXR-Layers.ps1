# Implicit OpenXR API layers (ReShade, OBS Mirror, ...) that stop GRID 2's 32-bit
# loader from creating an instance. Same rules as DiRT2VR: a bypass is offered only
# for a layer the loader named as unloadable, through the disable switch its own
# installed manifest declares. Nothing outside GRID2VR's processes is changed.
$script:Grid2VrImplicitLayers='SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

function Test-Grid2VrLayerDisableVariable([string]$Name) {
    # Never let a manifest turn a bypass into PATH, runtime, or other process configuration.
    return $Name -cmatch '\ADISABLE_(?:XR_|OPENXR_)[A-Za-z0-9_]{1,180}\z'
}

function Read-Grid2VrLayerManifest([string]$Path) {
    try {
        if(![IO.Path]::IsPathFullyQualified($Path)) { return $null }
        $file=Get-Item -LiteralPath $Path -Force -ErrorAction Stop
        if($file.PSIsContainer -or $file.Length -gt 64KB) { return $null }
        $bytes=[IO.File]::ReadAllBytes($file.FullName)
        $layer=([Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF) | ConvertFrom-Json -ErrorAction Stop).api_layer
        $name=[string]$layer.name; $library=[string]$layer.library_path; $variable=[string]$layer.disable_environment
        if($name -cnotmatch '\AXR_APILAYER_[A-Za-z0-9_]{1,200}\z' -or [string]::IsNullOrWhiteSpace($library) -or !(Test-Grid2VrLayerDisableVariable $variable)) { return $null }
        return [pscustomobject][ordered]@{layerName=$name;manifestPath=[IO.Path]::GetFullPath($Path)
            manifestHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))
            libraryPath=[IO.Path]::GetFullPath([IO.Path]::Combine([IO.Path]::GetDirectoryName($Path),$library));disableVariable=$variable}
    } catch { return $null }
}

function Get-Grid2VrApiLayers {
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($hive in [Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryHive]::CurrentUser) {
        try {
            # The worker is x64; the probe and game read the 32-bit view of both hives.
            $root=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,[Microsoft.Win32.RegistryView]::Registry32)
            try {
                $key=$root.OpenSubKey($script:Grid2VrImplicitLayers)
                if(!$key) { continue }
                try {
                    foreach($path in $key.GetValueNames()) {
                        if($key.GetValueKind($path) -eq [Microsoft.Win32.RegistryValueKind]::DWord -and [int]$key.GetValue($path) -eq 0) { $null=$paths.Add($path) }
                    }
                } finally { $key.Dispose() }
            } finally { $root.Dispose() }
        } catch [IO.IOException],[UnauthorizedAccessException],[Security.SecurityException] {
            # An incomplete inventory must not authorize any bypass.
            return @()
        }
    }
    return @($paths | ForEach-Object { Read-Grid2VrLayerManifest $_ } | Where-Object { $_ })
}

function Get-Grid2VrLayerFailures([int]$ExitCode,[string]$Report) {
    $result=@()
    if($ExitCode -eq 0 -or $Report -cnotmatch '(?m)^create_instance=-32\r?$' -or !$Report.Contains('Failed loading layer information',[StringComparison]::Ordinal)) { return $result }
    # Loader text is from the pinned SDK. Match numeric Windows errors, not localized descriptions.
    foreach($match in [regex]::Matches($Report,'ApiLayerInterface::LoadApiLayers skipping layer (?<name>XR_APILAYER_[A-Za-z0-9_]+), failed to load with message "Failed to open dynamic library (?<path>[^\r\n]+?) with error (?<error>[0-9]+):')) {
        $code=0
        if(![int]::TryParse($match.Groups['error'].Value,[ref]$code) -or ![IO.Path]::IsPathFullyQualified($match.Groups['path'].Value)) { continue }
        try { $path=[IO.Path]::GetFullPath($match.Groups['path'].Value) } catch [ArgumentException] { continue }
        $name=$match.Groups['name'].Value
        if(@($result | Where-Object { $_.layerName -ceq $name -and [string]::Equals($_.libraryPath,$path,[StringComparison]::OrdinalIgnoreCase) }).Count) { continue }
        $result+=[pscustomobject][ordered]@{layerName=$name;libraryPath=$path;windowsError=$code}
    }
    return $result
}

function Resolve-Grid2VrLayerBypasses([object[]]$Failures,[object[]]$Catalog) {
    $Failures=@($Failures | Where-Object { $_ }); $Catalog=@($Catalog | Where-Object { $_ })
    if($Failures.Count -eq 0 -or $Failures.Count -gt 16) { return @() }
    $result=@()
    foreach($failure in $Failures) {
        $found=@($Catalog | Where-Object { $_.layerName -ceq $failure.layerName })
        if($found.Count -ne 1 -or ![string]::Equals($found[0].libraryPath,$failure.libraryPath,[StringComparison]::OrdinalIgnoreCase) -or
           !(Test-Grid2VrLayerDisableVariable $found[0].disableVariable)) { return @() }
        # A shared switch would also disable a layer not named by the failure.
        if(@($Catalog | Where-Object { [string]::Equals($_.disableVariable,$found[0].disableVariable,[StringComparison]::OrdinalIgnoreCase) -and $_.layerName -cne $found[0].layerName }).Count) { return @() }
        $result+=$found[0]
    }
    return $result
}

function Test-Grid2VrLayerIdentity($Layer,$Other) {
    return [bool]($Layer -and $Other -and $Layer.layerName -ceq $Other.layerName -and $Layer.manifestHash -ceq $Other.manifestHash -and $Layer.disableVariable -ceq $Other.disableVariable -and
        [string]::Equals($Layer.manifestPath,$Other.manifestPath,[StringComparison]::OrdinalIgnoreCase) -and [string]::Equals($Layer.libraryPath,$Other.libraryPath,[StringComparison]::OrdinalIgnoreCase))
}

# Keep only approved bypasses whose layer registration is unchanged since approval.
function Select-Grid2VrLayerBypasses([object[]]$Approved,[object[]]$Catalog) {
    $result=@()
    foreach($entry in @($Approved | Where-Object { $_ })) {
        $resolved=@(Resolve-Grid2VrLayerBypasses @([pscustomobject]@{layerName=$entry.layerName;libraryPath=$entry.libraryPath}) $Catalog)
        if($resolved.Count -eq 1 -and (Test-Grid2VrLayerIdentity $resolved[0] $entry) -and !@($result | Where-Object { Test-Grid2VrLayerIdentity $_ $entry }).Count) { $result+=$resolved[0] }
    }
    return $result
}

function ConvertFrom-Grid2VrLayerTransport([string]$Base64) {
    if(!$Base64) { return @() }
    if($Base64.Length -gt 16384) { throw 'OpenXR layer approvals exceed their size limit.' }
    $entries=@([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Base64)) | ConvertFrom-Json -ErrorAction Stop)
    if($entries.Count -gt 16) { throw 'Too many OpenXR layer approvals.' }
    return @($entries | ForEach-Object {
        [pscustomobject][ordered]@{layerName=[string]$_.layerName;manifestPath=[string]$_.manifestPath;manifestHash=[string]$_.manifestHash;libraryPath=[string]$_.libraryPath;disableVariable=[string]$_.disableVariable}
    })
}

function Format-Grid2VrLayerFailures([object[]]$Failures) {
    return 'An OpenXR layer could not load: '+((@($Failures) | ForEach-Object { "$($_.layerName) (Windows error $($_.windowsError))" }) -join '; ')+'.'
}

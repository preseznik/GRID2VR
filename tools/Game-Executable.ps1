param([string]$Grid2ExecutablePath, [ValidateSet('Avx','Regular')][string]$Grid2ExecutableVariant='Avx')

function Get-Grid2ExecutableHash([string]$Path,[ValidateSet('Avx','Regular')][string]$Variant='Avx') {
    if(!('GRID2VR.GameExecutable' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'GameExecutable.cs') -ErrorAction Stop
    }
    return [GRID2VR.GameExecutable]::Validate($Path, $Variant -eq 'Avx')
}

function Get-Grid2LaunchBackend([string]$Game) {
    if(!('GRID2VR.GameExecutable' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'GameExecutable.cs') -ErrorAction Stop
    }
    return [GRID2VR.GameExecutable]::ValidateInstallation($Game)
}

# GRID 2 resolves its static d3d11.dll import to the proxy in the game folder. If
# that file is gone (often antivirus) or cannot load (a missing runtime DLL or
# entry point), Windows' own d3d11.dll is used and the mod silently does nothing.
# Record which, once, when the game window first exists.
function Write-RendererLoad([string]$Game,[string]$Run,[int]$ProcessId,[string]$ProxySha256) {
    $proxy=[IO.Path]::GetFullPath((Join-Path $Game 'd3d11.dll'))
    $present=Test-Path -LiteralPath $proxy -PathType Leaf
    $unchanged=$present -and (Get-FileHash -LiteralPath $proxy).Hash -eq $ProxySha256
    $loaded=@();$inspected=$true;$preferSystem32=$null
    try {
        if(!('GRID2VR.GameProcessModules' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'GameExecutable.cs') -ErrorAction Stop }
        $loaded=@([GRID2VR.GameProcessModules]::Paths($ProcessId,'d3d11.dll'))
    } catch { $inspected=$false }
    try { $preferSystem32=[GRID2VR.GameProcessModules]::PrefersSystem32Images($ProcessId) } catch { }
    $rendererLoaded=@($loaded | Where-Object { [string]::Equals([IO.Path]::GetFullPath($_),$proxy,[StringComparison]::OrdinalIgnoreCase) }).Count -gt 0
    $reason=if(!$inspected){'not_inspected'}elseif($rendererLoaded){'loaded'}elseif(!$present){'proxy_missing'}elseif(!$unchanged){'proxy_changed'}elseif($preferSystem32){'prefer_system32_policy'}else{'windows_d3d11_loaded'}
    $ancestry=@();try { $ancestry=@(Get-PreferSystem32Ancestry $ProcessId) } catch { }
    $result=[ordered]@{schema=2;rendererLoaded=$rendererLoaded;reason=$reason;proxyPresent=[bool]$present;proxyUnchanged=[bool]$unchanged;loadedD3d11=@($loaded);preferSystem32Images=$preferSystem32
        ancestry=$ancestry;ruleOrigin=$(if($reason -ceq 'prefer_system32_policy'){Get-PreferSystem32Origin $ancestry}else{$null})}
    [IO.File]::WriteAllText((Join-Path $Run 'renderer-load.json'),($result | ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
    return $result
}

# Windows' "prefer system32 images" rule (exploit protection, PreferSystem32) makes
# GRID 2 take Windows' d3d11.dll over GRID2VR's. A process passes the rule to every
# process it starts and a child cannot drop it, so Steam carrying it is enough.
# A per-program setting is the Image File Execution Options MitigationOptions
# value: bits 60-61 of its first qword are 1 when the rule is forced on.
function Test-PreferSystem32Option($Options) {
    return $Options -is [byte[]] -and $Options.Length -ge 8 -and (([BitConverter]::ToUInt64($Options,0) -shr 60) -band 3) -eq 1
}
function Test-PreferSystem32Setting([string]$Path) {
    if(!$Path) { return $false }
    $key=Join-Path 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options' ([IO.Path]::GetFileName($Path))
    if(!(Test-Path -LiteralPath $key)) { return $false }
    if(Test-PreferSystem32Option (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).MitigationOptions) { return $true }
    foreach($filter in @(Get-ChildItem -LiteralPath $key -ErrorAction SilentlyContinue)) {
        $value=Get-ItemProperty -LiteralPath $filter.PSPath -ErrorAction SilentlyContinue
        if([string]::Equals([string]$value.FilterFullPath,$Path,[StringComparison]::OrdinalIgnoreCase) -and (Test-PreferSystem32Option $value.MitigationOptions)) { return $true }
    }
    return $false
}
# The game process and its live ancestors (normally grid2.exe and Steam), nearest first.
function Get-PreferSystem32Ancestry([int]$ProcessId) {
    if(!('GRID2VR.GameProcessModules' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'GameExecutable.cs') -ErrorAction Stop }
    foreach($link in [GRID2VR.GameProcessModules]::Ancestry($ProcessId,8)) {
        [ordered]@{id=$link.Id;name=$link.Name;path=$link.Path;preferSystem32Images=$link.PreferSystem32Images
            exploitProtectionSetting=[bool](Test-PreferSystem32Setting $link.Path)}
    }
}
# Where the rule came from: the farthest process in the unbroken run, from the game
# upward, that carries it. startedBy is its parent when that is still running.
function Get-PreferSystem32Origin($Ancestry) {
    $links=@($Ancestry);$index=-1
    while($index+1 -lt $links.Count -and $links[$index+1].preferSystem32Images -eq $true) { ++$index }
    if($index -lt 0) { return $null }
    $origin=$links[$index]
    return [ordered]@{name=$origin.name;path=$origin.path;exploitProtectionSetting=$origin.exploitProtectionSetting
        startedBy=$(if($index+1 -lt $links.Count){$links[$index+1].name}else{$null})}
}
# Before launch: the process that will start GRID 2 must not carry the rule. With
# Steam that is the running Steam client, or this process when Steam must start.
function Get-PreferSystem32LaunchBlock([ValidateSet('Steam','Direct')][string]$Backend,[string]$Game,[string]$SteamRoot) {
    if(!('GRID2VR.GameProcessModules' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'GameExecutable.cs') -ErrorAction Stop }
    $starters=@()
    if($Backend -eq 'Steam') {
        $steam=[IO.Path]::GetFullPath((Join-Path $SteamRoot 'steam.exe'))
        $starters=@(Get-Process steam -ErrorAction SilentlyContinue | Where-Object { [string]::Equals($_.Path,$steam,[StringComparison]::OrdinalIgnoreCase) })
        foreach($path in $steam,(Join-Path $Game 'grid2.exe'),(Join-Path $Game 'grid2_avx.exe')) {
            if(Test-PreferSystem32Setting $path) { return [ordered]@{name=[IO.Path]::GetFileName($path);path=$path;exploitProtectionSetting=$true;running=$false} }
        }
    } elseif(Test-PreferSystem32Setting (Join-Path $Game 'grid2_avx.exe')) {
        return [ordered]@{name='grid2_avx.exe';path=(Join-Path $Game 'grid2_avx.exe');exploitProtectionSetting=$true;running=$false}
    }
    if(!$starters.Count) { $starters=@(Get-Process -Id $PID) }
    foreach($process in $starters) {
        $prefer=$null;try { $prefer=[GRID2VR.GameProcessModules]::PrefersSystem32Images($process.Id) } catch { }
        if($prefer) { return [ordered]@{name=[IO.Path]::GetFileName($process.Path);path=$process.Path;exploitProtectionSetting=$false;running=$process.Id -ne $PID} }
    }
    return $null
}
if($Grid2ExecutablePath) {
    $ErrorActionPreference='Stop'
    try { Get-Grid2ExecutableHash $Grid2ExecutablePath $Grid2ExecutableVariant }
    catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
}

. (Join-Path $PSScriptRoot 'Paths.ps1')
. (Join-Path $PSScriptRoot 'Game-Executable.ps1')
# Managed careers never replace the ordinary Steam save directory.
function Get-ProfileAccountId {
    param([switch]$AllowUnavailable)
    try { $value=(Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam\ActiveProcess' -Name ActiveUser -ErrorAction Stop).ActiveUser }
    catch { if($AllowUnavailable) { return $null }; throw }
    $account=[BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$value),0)
    if(!$account -and $AllowUnavailable) { return $null }
    if(!$account) { throw 'Sign into Steam before using a managed career.' }
    return $account.ToString([Globalization.CultureInfo]::InvariantCulture)
}

function Read-ProfileJson {
    param([string]$Path)
    $null=Assert-PlainPath $Path
    if(!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 8192) { throw 'Career metadata is missing or too large.' }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop
}

function Get-LaunchProfile {
    param([string]$Root,[string]$Id='', [string]$AccountId='')
    if(!$Id) {
        $selection=Join-Path (Get-Grid2DataRoot $Root) 'profile-selection.json'
        $null=Assert-PlainPath $selection
        if(!(Test-Path -LiteralPath $selection)) { $Id='current' }
        else {
            $choice=Read-ProfileJson $selection
            if($choice.schema -ne 1 -or !$choice.id) { throw 'Career selection is invalid. Select a profile in the launcher.' }
            $Id=[string]$choice.id
        }
    }
    if($Id -ceq 'current') { return @{id='current';name='Current game career';remote=$null;accountId=$null} }
    if($Id -cnotmatch '^[0-9a-f]{32}$') { throw 'Invalid career identifier.' }
    if((Get-Grid2LaunchBackend (Get-Grid2GameRoot $Root)) -eq 'Direct') {
        throw 'Standalone installations use Current game career. Select it in the launcher; separate launcher profiles are unavailable.'
    }
    $folder=Join-Path (Get-Grid2DataRoot $Root) "profiles/$Id"
    $info=Read-ProfileJson (Join-Path $folder 'profile.json')
    if(!$AccountId) {
        $AccountId=Get-ProfileAccountId -AllowUnavailable
        # Steam can be logged in while ActiveProcess is zero. An existing career
        # supplies the expected owner; the native provider still compares the
        # actual SteamUser account before returning either managed save interface.
        if(!$AccountId) { $AccountId=[string]$info.accountId }
    }
    if($AccountId -notmatch '^[0-9]{1,10}$' -or [uint64]$AccountId -eq 0 -or [uint64]$AccountId -gt [uint32]::MaxValue) { throw 'Invalid Steam account for this career.' }
    if($info.schema -ne 1 -or $info.id -cne $Id -or $info.name -cnotmatch '^[A-Za-z0-9]{1,24}$' -or
       $info.kind -notin @('fresh','copied') -or [string]$info.accountId -cne $AccountId) {
        throw 'Career metadata is invalid or belongs to a different Steam account.'
    }
    $remote=[IO.Path]::GetFullPath((Join-Path $folder 'remote'))
    $null=Assert-PlainPath $remote
    if(!(Test-Path -LiteralPath $remote -PathType Container)) { throw 'Career save folder is missing.' }
    foreach($entry in Get-ChildItem -LiteralPath $folder -Force) {
        $null=Assert-PlainPath $entry.FullName
        if($entry.Name -cnotin @('profile.json','remote','local')) { throw 'Unexpected file in career folder.' }
    }
    $files=@(Get-ChildItem -LiteralPath $remote -Force)
    $local=Join-Path $folder 'local'
    $null=Assert-PlainPath $local
    if(Test-Path -LiteralPath $local) {
        if(!(Test-Path -LiteralPath $local -PathType Container)) { throw 'Local career folder is invalid.' }
        # Enumerate one level at a time, rejecting links before descending.
        $queue=[Collections.Generic.Queue[object]]::new(); $queue.Enqueue(@($local,0))
        $directories=0
        while($queue.Count) {
            $item=$queue.Dequeue()
            foreach($entry in Get-ChildItem -LiteralPath $item[0] -Force) {
                $null=Assert-PlainPath $entry.FullName
                if($entry.PSIsContainer) {
                    if($item[1] -ge 4 -or ++$directories -gt 16) { throw 'Local career directory limit exceeded.' }
                    $queue.Enqueue(@($entry.FullName,($item[1]+1)))
                } else { $files+=@($entry) }
            }
        }
    }
    if($files.Count -gt 64) { throw 'Career exceeds the 64-file limit.' }
    $bytes=0L
    foreach($file in $files) {
        $null=Assert-PlainPath $file.FullName
        if($file.PSIsContainer -or $file.Name.Length -gt 180 -or $file.Name -cmatch '[<>:"/\\|?*\x00-\x1f\x7f-\uffff]' -or
           $file.Name -match '[. ]$' -or $file.BaseName -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') { throw 'Unsupported career save file.' }
        $bytes+=$file.Length
    }
    if($bytes -gt 64MB) { throw 'Career exceeds the 64 MiB limit.' }
    return @{id=$Id;name=[string]$info.name;remote=$remote;accountId=$AccountId}
}

function Get-CareerProfileProxy {
    param([string]$Root,[string]$BaseProxy)
    # Installed releases use their manifest-verified renderer. Development
    # candidates still require one of the explicitly qualified binary hashes.
    $allowed=@('9fe9e76f825b34650b64d962fc2e115d4fe09c9190eef0decc90362fbd46fe22',
        'fa75274d189e5121cc0e35ccbbc2e2df38b1fee3ef3d5dec3a6b73f559c47495',
        'cdd6e68abcd6e53ee15876f86f952b0e808b2fe515fd443ad80fb05f24fa5da8')
    $null=Assert-PlainPath $BaseProxy
    $base=(Get-FileHash -LiteralPath $BaseProxy).Hash
    $packaged=Test-Grid2Package $Root
    $proxy=if($packaged){Get-Grid2Proxy $Root}else{$BaseProxy}
    $null=Assert-PlainPath $proxy
    if((!$packaged -and $base -notin $allowed) -or (Get-FileHash -LiteralPath $proxy -ErrorAction Stop).Hash -ne $base) {
        throw 'The managed career provider is missing or differs from its verified renderer. Rebuild the profile package.'
    }
    return $proxy
}

function Write-ProfileStorageRequest {
    param([hashtable]$Profile,[string]$Run)
    if($Profile.id -eq 'current') { return }
    $null=Assert-PlainPath $Run
    $request=Join-Path $Run 'profile-storage.request'
    $text="GRID2VR_PROFILE_STORAGE_V1`n$($Profile.remote)`n$($Profile.id)`n$($Profile.accountId)`n"
    [IO.File]::WriteAllText($request,$text,[Text.UTF8Encoding]::new($false))
}

function Assert-CareerProfileSession {
    param([hashtable]$Profile,[string]$Run,[string]$Root)
    if($Profile.id -eq 'current') { return }
    $path=Join-Path $Run 'profile-storage.json'
    if(!(Test-Path -LiteralPath $path)) { throw "Managed career storage did not start. Inspect logs in: $Run" }
    $state=Read-ProfileJson $path
    if($state.phase -ne 'active' -or $state.profileId -cne $Profile.id -or $state.steamStorageCalls -ne 0) {
        throw "Managed career storage refused this session: $($state.reason). Logs: $Run"
    }
    if($state.reason -or $state.writeFailures -gt 0) {
        throw "Managed career could not save successfully: $($state.reason). Logs: $Run"
    }
    # Validate only; never roll career progress back during recovery.
    $null=Get-LaunchProfile -Root $Root -Id $Profile.id -AccountId $Profile.accountId
}

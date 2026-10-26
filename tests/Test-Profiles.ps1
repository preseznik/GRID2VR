$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/User-Session.ps1')
. (Join-Path $root 'tools/Profiles.ps1')
$temp=Join-Path $root ('build/profiles-test-'+[Guid]::NewGuid().ToString('N'))
function Check($value,$message) { if(!$value) { throw $message } }
function Refuses([scriptblock]$action) {
    $failed=$false; try { & $action | Out-Null } catch { $failed=$true }
    Check $failed 'Unsafe career request was accepted.'
}
try {
    New-Item -ItemType Directory -Path $temp | Out-Null
    # Texture selection must not force managed careers back to an older renderer.
    $legacy=Join-Path $root 'build/profile-storage-candidate/GRID2VR.dll'
    $preview=Join-Path $root 'build/visibility-headset-candidate/GRID2VR.dll'
    foreach($renderer in @($legacy,$preview)) {
        Check ((Get-CareerProfileProxy -Root $temp -BaseProxy $renderer) -eq $renderer) 'Managed career changed the verified renderer.'
    }
    $foreign=Join-Path $temp 'foreign.dll';[IO.File]::WriteAllText($foreign,'foreign')
    Refuses { Get-CareerProfileProxy -Root $temp -BaseProxy $foreign }
    $payload=Join-Path $temp 'payload';New-Item -ItemType Directory -Path $payload | Out-Null
    $installed=Join-Path $payload 'd3d11.dll';Copy-Item -LiteralPath $preview -Destination $installed
    Write-SessionJson (Join-Path $temp 'package.json') @{schema=1;files=@{'GRID2VR/payload/d3d11.dll'=(Get-FileHash -LiteralPath $installed).Hash.ToLowerInvariant()}}
    Check ((Get-CareerProfileProxy -Root $temp -BaseProxy $preview) -eq $installed) 'Packaged career ignored the verified core file.'
    Refuses { Get-CareerProfileProxy -Root $temp -BaseProxy $legacy }
    # A rebuilt release is qualified by the package manifest, not stale dev pins.
    [IO.File]::WriteAllText($installed,'new release renderer fixture')
    Write-SessionJson (Join-Path $temp 'package.json') @{schema=1;files=@{'GRID2VR/payload/d3d11.dll'=(Get-FileHash -LiteralPath $installed).Hash.ToLowerInvariant()}}
    Check ((Get-CareerProfileProxy -Root $temp -BaseProxy $installed) -eq $installed) 'New manifest-verified release renderer was rejected.'
    Refuses { Get-CareerProfileProxy -Root $temp -BaseProxy $preview }
    [IO.File]::WriteAllText($installed,'tampered')
    Refuses { Get-CareerProfileProxy -Root $temp -BaseProxy $installed }
    Refuses { Get-CareerProfileProxy -Root $temp -BaseProxy $preview }
    [IO.File]::Delete((Join-Path $temp 'package.json'))
    Check ((Get-LaunchProfile $temp).id -ceq 'current') 'Default must keep current career.'
    $id=[Guid]::NewGuid().ToString('N')
    $profile=Join-Path $temp "artifacts/profiles/$id"
    $remote=Join-Path $profile 'remote'; $local=Join-Path $profile 'local/savegame'
    New-Item -ItemType Directory -Path $remote,$local | Out-Null
    $info=@{schema=1;id=$id;name='TestCareer';kind='fresh';createdUtc=[DateTime]::UtcNow.ToString('o');accountId='123'}
    Write-SessionJson (Join-Path $profile 'profile.json') $info
    Write-SessionJson (Join-Path $temp 'artifacts/profile-selection.json') @{schema=1;id=$id}
    $career=Get-LaunchProfile -Root $temp -AccountId '123'
    Check ($career.remote -ceq $remote) 'Selected career did not route to its own remote folder.'
    Write-ProfileStorageRequest $career $temp
    $lines=[IO.File]::ReadAllLines((Join-Path $temp 'profile-storage.request'))
    Check ($lines.Count -eq 4 -and $lines[0] -ceq 'GRID2VR_PROFILE_STORAGE_V1' -and $lines[1] -ceq $remote -and $lines[2] -ceq $id -and $lines[3] -ceq '123') 'Native career request does not match selected profile.'
    Refuses { Assert-CareerProfileSession $career $temp $temp }
    Write-SessionJson (Join-Path $temp 'profile-storage.json') @{schema=1;phase='active';profileId=$id;steamStorageCalls=0}
    Assert-CareerProfileSession $career $temp $temp
    Write-SessionJson (Join-Path $temp 'profile-storage.json') @{schema=1;phase='active';profileId=$id;steamStorageCalls=0;reason='write_failed'}
    Refuses { Assert-CareerProfileSession $career $temp $temp }
    Write-SessionJson (Join-Path $temp 'profile-storage.json') @{schema=1;phase='active';profileId=$id;steamStorageCalls=0;reason='';writes=2;writeFailures=1}
    Refuses { Assert-CareerProfileSession $career $temp $temp }
    Write-SessionJson (Join-Path $temp 'profile-storage.json') @{schema=1;phase='active';profileId='different';steamStorageCalls=0}
    Refuses { Assert-CareerProfileSession $career $temp $temp }
    Write-SessionJson (Join-Path $temp 'profile-storage.json') @{schema=1;phase='refused';reason='unsupported';profileId=$id;steamStorageCalls=0}
    Refuses { Assert-CareerProfileSession $career $temp $temp }
    Refuses { Get-LaunchProfile -Root $temp -AccountId '456' }
    $originalAccountReader=${function:Get-ProfileAccountId}
    try {
        function Get-ProfileAccountId { param([switch]$AllowUnavailable); return $null }
        Check ((Get-LaunchProfile -Root $temp).accountId -ceq '123') 'Existing career must retain its expected owner when Steam registry is unavailable.'
        function Get-ProfileAccountId { param([switch]$AllowUnavailable); return '456' }
        Refuses { Get-LaunchProfile -Root $temp }
        $info.accountId='0'; Write-SessionJson (Join-Path $profile 'profile.json') $info
        function Get-ProfileAccountId { param([switch]$AllowUnavailable); return $null }
        Refuses { Get-LaunchProfile -Root $temp }
        $info.accountId='123'; Write-SessionJson (Join-Path $profile 'profile.json') $info
    } finally { ${function:Get-ProfileAccountId}=$originalAccountReader }
    Refuses { Get-LaunchProfile -Root $temp -Id '../remote' -AccountId '123' }
    Refuses { Get-LaunchProfile -Root $temp -Id 'CURRENT' }
    [IO.File]::WriteAllText((Join-Path $local 'savegame@profile0#test'), 'local fallback')
    Check ((Get-LaunchProfile -Root $temp -AccountId '123').id -ceq $id) 'Native local fallback rejected.'
    New-Item -ItemType Directory -Path (Join-Path $remote 'nested') | Out-Null
    Refuses { Get-LaunchProfile -Root $temp -AccountId '123' }
    Remove-Item -LiteralPath (Join-Path $remote 'nested')
    [IO.File]::WriteAllText((Join-Path $profile 'unexpected.txt'), 'not owned')
    Refuses { Get-LaunchProfile -Root $temp -AccountId '123' }
    Remove-Item -LiteralPath (Join-Path $profile 'unexpected.txt')
    [IO.File]::WriteAllText((Join-Path $temp 'artifacts/profile-selection.json'), '{broken')
    Refuses { Get-LaunchProfile -Root $temp -AccountId '123' }
    Check ((Get-LaunchProfile -Root $temp -Id 'current').id -ceq 'current') 'Explicit current selection must remain possible.'
    foreach($script in @('Start-UserSession.ps1','Run-StereoProbe.ps1','Run-Diagnostic.ps1','Profiles.ps1')) {
        $tokens=$null; $errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$script"),[ref]$tokens,[ref]$errors)
        Check (!$errors) "Invalid PowerShell syntax: $script"
        if($script -in @('Run-StereoProbe.ps1','Run-Diagnostic.ps1')) {
            Check (@($ast.ParamBlock.Parameters.Name.VariablePath.UserPath) -contains 'CareerProfile') 'Career selection not forwarded.'
        }
    }
    'Career routing, native request, account ownership and malformed selection checks passed.'
} finally {
    $absolute=[IO.Path]::GetFullPath($temp)
    if([IO.Path]::GetDirectoryName($absolute) -ne (Join-Path $root 'build')) { throw 'Invalid profile fixture cleanup path.' }
    if(Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}

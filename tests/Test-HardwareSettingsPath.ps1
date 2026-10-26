param([string]$CloudSettings='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'tools/User-Session.ps1')
$fixture=Join-Path $root ('artifacts/hardware-settings-test-'+[guid]::NewGuid().ToString('N'))
function Check($Condition,[string]$Message) { if(!$Condition) { throw $Message } }
function RejectLink([string]$Path) {
    $message=''
    try { Assert-HardwareSettingsPath $Path | Out-Null } catch { $message=$_.Exception.Message }
    Check ($message -like '*Linked hardware settings retained:*') 'Real link was not refused.'
}
try {
    $plain=Join-Path $fixture 'plain'
    New-Item -ItemType Directory -Path $plain | Out-Null
    $config=Join-Path $plain 'hardware_settings_config.xml'
    [IO.File]::WriteAllText($config,'original settings')
    Check ((Assert-HardwareSettingsPath $config) -eq $config) 'Plain settings rejected.'
    foreach($variant in 0..15) {
        $tag=[uint32]0x9000001Au -bor ([uint32]$variant -shl 12)
        Check ([GRID2VR.HardwareSettingsPath]::IsCloudTag($tag)) 'Cloud Files variant rejected.'
    }
    foreach($tag in @([uint32]0, [uint32]0xA000000Cu, [uint32]0xA0000003u, [uint32]0x80000021u, [uint32]0x9001001Au, [uint32]0xB000001Au)) {
        Check (![GRID2VR.HardwareSettingsPath]::IsCloudTag($tag)) 'Non-cloud or name-surrogate tag accepted.'
    }
    $hash=(Get-FileHash -LiteralPath $config).Hash
    foreach($kind in @('Junction')) {
        $link=Join-Path $fixture ('linked-'+$kind)
        New-Item -ItemType $kind -Path $link -Target $plain | Out-Null
        try {
            RejectLink $link
            RejectLink (Join-Path $link 'hardware_settings_config.xml')
        } finally { [IO.Directory]::Delete($link) } # Only unlink the fixture, never recurse into its target.
    }
    Check ((Get-FileHash -LiteralPath $config).Hash -eq $hash) 'Link checks changed the original.'
    if($CloudSettings) {
        $tag=[GRID2VR.HardwareSettingsPath]::ReadTag($CloudSettings)
        Check ([GRID2VR.HardwareSettingsPath]::IsCloudTag($tag)) 'Expected a real Cloud Files folder.'
        Assert-HardwareSettingsPath $CloudSettings | Out-Null
        foreach($name in @('hardware_settings_config.xml','hardware_settings_info.xml')) {
            $path=Join-Path $CloudSettings $name
            Assert-HardwareSettingsPath $path | Out-Null
        }
        $strictRejected=$false
        try { Assert-PlainPath $CloudSettings | Out-Null } catch { $strictRejected=$true }
        Check $strictRejected 'Cloud exception leaked into general path validation.'
        Write-Output ('Read-only Cloud Files metadata checks passed; tag 0x'+$tag.ToString('X8')+'. Content availability is checked when backing up settings.')
    }
    Write-Output 'Hardware settings paths: plain files, 16 cloud tags, foreign/symlink tags, junctions and ancestor guards passed.'
} finally {
    $resolved=Get-SessionChild (Join-Path $root 'artifacts') $fixture '^hardware-settings-test-[0-9a-f]{32}$'
    if(Test-Path -LiteralPath $resolved) {
        if(@(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}).Count) { throw 'Linked fixture retained.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

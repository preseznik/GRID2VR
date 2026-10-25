function Assert-RecenterKey([int]$Key,[int]$Modifiers) {
    $valid=($Key -ge 48 -and $Key -le 90) -or ($Key -ge 96 -and $Key -le 135) -or
        $Key -in @(8,9,13,19,32,33,34,35,36,37,38,39,40,45,46)
    if(!$valid -or $Modifiers -lt 0 -or $Modifiers -gt 7 -or
        (($Modifiers -band 2) -and $Key -in @(9,115)) -or ($Key -eq 46 -and ($Modifiers -band 3) -eq 3)) {
        throw 'Invalid recenter shortcut. Use a key with optional Ctrl, Alt or Shift; system shortcuts are reserved.'
    }
}

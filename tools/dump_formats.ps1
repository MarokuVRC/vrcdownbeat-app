# Dumps the shared-mode device format (from the endpoint property store)
# for every render endpoint, plus its name and state.
$base = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
Get-ChildItem $base | ForEach-Object {
    $state = (Get-ItemProperty $_.PSPath).DeviceState
    $props = Get-ItemProperty "$($_.PSPath)\Properties" -ErrorAction SilentlyContinue
    $name = $props."{a45c254e-df1c-4efd-8020-67d146a850e0},2"
    if ($state -ne 1) { return }   # active endpoints only

    # Device format: PKEY_AudioEngine_DeviceFormat {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0
    $fmt = $props."{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0"
    $line = "[ACTIVE] $name"
    if ($fmt -is [byte[]] -and $fmt.Length -ge 16) {
        $tag      = [BitConverter]::ToUInt16($fmt, 0)
        $channels = [BitConverter]::ToUInt16($fmt, 2)
        $rate     = [BitConverter]::ToUInt32($fmt, 4)
        $bits     = [BitConverter]::ToUInt16($fmt, 14)
        $line += "  ->  tag=$tag channels=$channels rate=$rate bits=$bits"
    } else {
        $line += "  ->  (no device format property)"
    }
    Write-Output $line
}

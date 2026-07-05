# Prints the Windows "Listen to this device" state of all CABLE capture endpoints.
$base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
Get-ChildItem $base | ForEach-Object {
    $p = Join-Path $_.PSPath 'Properties'
    if (Test-Path $p) {
        $props  = Get-ItemProperty $p
        $name   = $props.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
        $listen = $props.'{24dbb0fc-9311-4b3d-9cf0-18ff155639d4},1'
        $target = $props.'{24dbb0fc-9311-4b3d-9cf0-18ff155639d4},0'
        if ($name -like '*CABLE*') {
            Write-Output ("{0} | listen raw: {1} | target: {2}" -f $name, ($listen -join ','), $target)
        }
    }
}

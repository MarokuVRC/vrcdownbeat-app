param([string]$NameFilter = "*")

$base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'

Get-ChildItem $base | ForEach-Object {
    $props = Get-ItemProperty ($_.PSPath + '\Properties') -ErrorAction SilentlyContinue
    if ($props -eq $null) { return }
    $name   = $props.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $listen = $props.'{24dbb0fc-9311-4b3d-9cf0-18ff155639d4},1'
    if ($name -like $NameFilter) {
        $raw = if ($listen -eq $null) { '(not set)' } else { ($listen | ForEach-Object { $_ }) -join ',' }
        Write-Output ("{0} -> listen bytes: {1}" -f $name, $raw)
    }
}

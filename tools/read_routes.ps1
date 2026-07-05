$base = 'HKCU:\Software\Microsoft\Multimedia\Audio\DefaultEndpoint'
if (-not (Test-Path $base)) { Write-Output "no DefaultEndpoint key"; exit 0 }
Get-ChildItem $base | ForEach-Object {
    $name = $_.PSChildName
    $props = Get-ItemProperty $_.PSPath
    $props.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object {
        Write-Output ("{0} | {1} = {2}" -f $name, $_.Name, $_.Value)
    }
}

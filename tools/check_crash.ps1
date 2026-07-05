Get-Process BandJam -ErrorAction SilentlyContinue | Select-Object Id, StartTime | Format-Table

Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000 } -MaxEvents 10 -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -like '*BandJam*' } |
    ForEach-Object {
        Write-Output ("--- " + $_.TimeCreated)
        Write-Output $_.Message.Substring(0, [Math]::Min(600, $_.Message.Length))
    }

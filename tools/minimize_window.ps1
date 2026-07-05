param([int]$ProcId, [int]$Cmd = 6)  # 6 = minimize, 9 = restore

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinMin {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@
$p = Get-Process -Id $ProcId
[WinMin]::ShowWindow([IntPtr]$p.MainWindowHandle, $Cmd) | Out-Null
Write-Output "done"

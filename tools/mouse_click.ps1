param([int]$X, [int]$Y)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Mouse2 {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public const uint DOWN = 0x0002, UP = 0x0004;
}
"@

[Mouse2]::SetCursorPos($X, $Y) | Out-Null
Start-Sleep -Milliseconds 150
[Mouse2]::mouse_event([Mouse2]::DOWN, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[Mouse2]::mouse_event([Mouse2]::UP, 0, 0, 0, [UIntPtr]::Zero)
Write-Output "clicked at $X,$Y"

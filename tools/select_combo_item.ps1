param([int]$ProcId, [int]$ComboIndex, [string]$ItemName)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinClick {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
}
"@

function Click-Point([int]$x, [int]$y) {
    [WinClick]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 150
    [WinClick]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [WinClick]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
}

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($win -eq $null) { Write-Output "window not found"; exit 1 }

$p = Get-Process -Id $ProcId
[WinClick]::SetForegroundWindow([IntPtr]$p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400

$comboCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::ComboBox)
$combos = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $comboCond)
$combo = $combos[$ComboIndex]
$r = $combo.Current.BoundingRectangle
Click-Point ([int]($r.X + $r.Width / 2)) ([int]($r.Y + $r.Height / 2))
Start-Sleep -Milliseconds 900

# Find the popup menu item by (substring) name; the popup is a separate
# top-level element of the process.
$menuCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::MenuItem)
$procWins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
$item = $null
foreach ($pw in $procWins) {
    $found = $pw.FindAll([System.Windows.Automation.TreeScope]::Descendants, $menuCond)
    foreach ($f in $found) {
        Write-Output "  candidate: '$($f.Current.Name)'"
        if ($f.Current.Name -like "*$ItemName*") { $item = $f; break }
    }
    if ($item -ne $null) { break }
}
if ($item -eq $null) { Write-Output "item '$ItemName' not found"; exit 1 }

$ir = $item.Current.BoundingRectangle
Click-Point ([int]($ir.X + $ir.Width / 2)) ([int]($ir.Y + $ir.Height / 2))
Start-Sleep -Milliseconds 600
Write-Output "selected '$ItemName'"

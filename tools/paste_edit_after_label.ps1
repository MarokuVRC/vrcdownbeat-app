param([int]$ProcId, [string]$Label, [string]$Text)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class NativeInput {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
}
"@

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($win -eq $null) { Write-Output "window not found"; exit 1 }

$txtCond = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Text)),
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Label)))
$lab = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $txtCond)
if ($lab -eq $null) { Write-Output "label '$Label' not found"; exit 1 }
$labRect = $lab.Current.BoundingRectangle

$editCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Edit)
$edits = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $editCond)

$target = $null
$bestDx = [double]::MaxValue
foreach ($e in $edits) {
    $r = $e.Current.BoundingRectangle
    $dy = [Math]::Abs(($r.Y + $r.Height/2) - ($labRect.Y + $labRect.Height/2))
    $dx = $r.X - $labRect.X
    if ($dy -lt 10 -and $dx -gt 0 -and $dx -lt $bestDx) { $target = $e; $bestDx = $dx }
}
if ($target -eq $null) { Write-Output "edit after '$Label' not found"; exit 1 }
$tr = $target.Current.BoundingRectangle

$p = Get-Process -Id $ProcId
[NativeInput]::SetForegroundWindow([IntPtr]$p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400

# Real mouse click into the middle of the editor.
$cx = [int]($tr.X + $tr.Width / 2); $cy = [int]($tr.Y + $tr.Height / 2)
[NativeInput]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep -Milliseconds 150
[NativeInput]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # left down
[NativeInput]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # left up
Start-Sleep -Milliseconds 300

# Layout-independent input: select all, paste from clipboard.
[System.Windows.Forms.Clipboard]::SetText($Text)
[System.Windows.Forms.SendKeys]::SendWait("^a")
Start-Sleep -Milliseconds 150
[System.Windows.Forms.SendKeys]::SendWait("^v")
Start-Sleep -Milliseconds 200
Write-Output "pasted '$Text' into edit after '$Label'"

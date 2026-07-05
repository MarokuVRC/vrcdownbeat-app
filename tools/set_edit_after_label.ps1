param([int]$ProcId, [string]$Label, [string]$Text)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($win -eq $null) { Write-Output "window not found"; exit 1 }

# Find the caption label.
$txtCond = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Text)),
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Label)))
$lab = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $txtCond)
if ($lab -eq $null) { Write-Output "label '$Label' not found"; exit 1 }
$labRect = $lab.Current.BoundingRectangle

# Nearest Edit control to the right of the label, on the same row.
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

Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class FgWin2 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@
$p = Get-Process -Id $ProcId
[FgWin2]::SetForegroundWindow([IntPtr]$p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400
$target.SetFocus()
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait("^a{DEL}")
Start-Sleep -Milliseconds 150
if ($Text -ne "") { [System.Windows.Forms.SendKeys]::SendWait($Text) }
Start-Sleep -Milliseconds 150
Write-Output "set '$Label' editor to '$Text'"

param([int]$ProcId, [string]$Text)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($win -eq $null) { Write-Output "window not found"; exit 1 }

# Find the Send button, then the Edit control on the same row (the chat input).
$btnCond = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)),
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, "Send")))
$send = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $btnCond)
if ($send -eq $null) { Write-Output "send button not found"; exit 1 }
$sendRect = $send.Current.BoundingRectangle

$editCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Edit)
$edits = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $editCond)

$input = $null
foreach ($e in $edits) {
    $r = $e.Current.BoundingRectangle
    if ([Math]::Abs(($r.Y + $r.Height/2) - ($sendRect.Y + $sendRect.Height/2)) -lt 8 -and $r.X -lt $sendRect.X) {
        $input = $e; break
    }
}
if ($input -eq $null) { Write-Output "chat input not found"; exit 1 }

# JUCE text editors don't support the UIA ValuePattern - focus + type instead.
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class FgWin {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@
$p = Get-Process -Id $ProcId
[FgWin]::SetForegroundWindow([IntPtr]$p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400
$input.SetFocus()
Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait($Text)
Start-Sleep -Milliseconds 200
[System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
Write-Output "sent '$Text'"

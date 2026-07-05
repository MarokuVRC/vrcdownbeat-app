param([int]$ProcId, [int]$ComboIndex, [string]$OutFile)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinCap2 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
}
"@

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($win -eq $null) { Write-Output "window not found"; exit 1 }

$p = Get-Process -Id $ProcId
[WinCap2]::SetForegroundWindow([IntPtr]$p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500

$comboCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::ComboBox)
$combos = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $comboCond)
$combo = $combos[$ComboIndex]
$r = $combo.Current.BoundingRectangle
$x = [int]($r.X + $r.Width / 2)
$y = [int]($r.Y + $r.Height / 2)
Write-Output "clicking combo $ComboIndex at $x,$y"

[WinCap2]::SetCursorPos($x, $y) | Out-Null
Start-Sleep -Milliseconds 200
[WinCap2]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)   # left down
[WinCap2]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)   # left up
Start-Sleep -Milliseconds 900

# Dump any menu/list items belonging to this process (JUCE popup menu).
$procCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$popups = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $procCond)
Write-Output "top-level elements of process: $($popups.Count)"
foreach ($pop in $popups) {
    $items = $pop.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $items) {
        $c = $e.Current
        $t = $c.ControlType.ProgrammaticName -replace 'ControlType\.',''
        if ($c.Name -and ($t -eq 'MenuItem' -or $t -eq 'ListItem' -or $t -eq 'Text')) {
            Write-Output "  [$t] $($c.Name)"
        }
    }
}

# Screenshot the full virtual screen area around the popup.
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($b.Width, $b.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved $OutFile"

# Close the popup again (Esc) so we don't change any setting.
[System.Windows.Forms.SendKeys]::SendWait("{ESC}")

param([int]$ProcId, [int]$SliderIndex = 1, [int]$DragDx = -60)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Mouse {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public const uint DOWN = 0x0002, UP = 0x0004, MOVE = 0x0001;
}
"@

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
if ($wins.Count -eq 0) { Write-Output "window not found"; exit 1 }

$sliderCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Slider)

foreach ($win in $wins) {
    $sliders = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $sliderCond)
    if ($sliders.Count -le $SliderIndex) { continue }
    $slider = $sliders[$SliderIndex]
    $r = $slider.Current.BoundingRectangle
    Write-Output ("slider rect: {0},{1} {2}x{3}" -f $r.X, $r.Y, $r.Width, $r.Height)

    # Value before
    $rv = $slider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    Write-Output ("value before: {0}" -f $rv.Current.Value)

    # Click in the middle of the track, then drag horizontally.
    $x = [int]($r.X + $r.Width * 0.5)
    $y = [int]($r.Y + $r.Height * 0.5)
    [Mouse]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 150
    [Mouse]::mouse_event([Mouse]::DOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    for ($i = 1; $i -le 10; $i++) {
        [Mouse]::SetCursorPos($x + [int]($DragDx * $i / 10), $y) | Out-Null
        Start-Sleep -Milliseconds 30
    }
    [Mouse]::mouse_event([Mouse]::UP, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 300

    $rv = $slider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    Write-Output ("value after: {0}" -f $rv.Current.Value)
    exit 0
}
Write-Output "slider index not found"
exit 1

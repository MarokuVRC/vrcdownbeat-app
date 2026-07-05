param([int]$ProcId, [int]$Index = 0, [double]$Value = 0)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

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
    if ($sliders.Count -eq 0) { continue }
    Write-Output "found $($sliders.Count) sliders"
    if ($Index -ge $sliders.Count) { Write-Output "index out of range"; exit 1 }
    $slider = $sliders[$Index]
    $rv = $slider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    Write-Output ("before: {0} (min {1}, max {2})" -f $rv.Current.Value, $rv.Current.Minimum, $rv.Current.Maximum)
    $rv.SetValue($Value)
    Start-Sleep -Milliseconds 300
    $rv = $slider.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
    Write-Output ("after: {0}" -f $rv.Current.Value)
    exit 0
}
Write-Output "no sliders found"
exit 1

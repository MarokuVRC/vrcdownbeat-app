param([int]$ProcId)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)

foreach ($win in $wins) {
    $btns = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Button)))
    foreach ($b in $btns) {
        $n = $b.Current.Name
        if ($n -eq 'You' -or $n -eq 'VRChat mic') {
            $r = $b.Current.BoundingRectangle
            $state = '?'
            try {
                $tp = $b.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
                $state = $tp.Current.ToggleState
            } catch {}
            Write-Output ("{0} | {1},{2} {3}x{4} | enabled={5} | toggle={6}" -f `
                $n, [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height, $b.Current.IsEnabled, $state)
        }
    }
}

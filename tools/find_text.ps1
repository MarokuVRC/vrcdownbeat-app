param([int]$ProcId, [string]$Filter)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
if ($wins.Count -eq 0) { Write-Output "window not found"; exit 1 }

foreach ($win in $wins) {
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($el in $all) {
        $name = $el.Current.Name
        if ($name -like "*$Filter*") {
            $r = $el.Current.BoundingRectangle
            Write-Output ("{0} | rect {1},{2} {3}x{4} | offscreen={5} | enabled={6}" -f
                $el.Current.ControlType.ProgrammaticName,
                [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height,
                $el.Current.IsOffscreen, $el.Current.IsEnabled)
        }
    }
}

param([int]$ProcId)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
foreach ($win in $wins) {
    $r = $win.Current.BoundingRectangle
    Write-Output ("'{0}': {1},{2} {3}x{4}" -f $win.Current.Name, [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height)
}

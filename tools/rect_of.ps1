param([int]$ProcId, [string]$Name, [string]$Type = "Button")

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
if ($wins.Count -eq 0) { Write-Output "window not found"; exit 1 }

$ct = [System.Windows.Automation.ControlType]::Button
if ($Type -eq "CheckBox") { $ct = [System.Windows.Automation.ControlType]::CheckBox }
if ($Type -eq "Slider")   { $ct = [System.Windows.Automation.ControlType]::Slider }

$c = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $ct)),
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Name)))
foreach ($win in $wins) {
    $els = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $c)
    foreach ($el in $els) {
        $r = $el.Current.BoundingRectangle
        Write-Output ("{0} '{1}': {2},{3} {4}x{5} offscreen={6}" -f $Type, $Name,
            [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height, $el.Current.IsOffscreen)
    }
}

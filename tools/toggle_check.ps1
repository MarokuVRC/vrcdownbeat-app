param([int]$ProcId, [string]$Name)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
if ($wins.Count -eq 0) { Write-Output "window not found"; exit 1 }

$boxCond = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::CheckBox)),
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Name)))
$box = $null
foreach ($win in $wins) {
    $box = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $boxCond)
    if ($box -ne $null) { break }
}
if ($box -eq $null) { Write-Output "checkbox '$Name' not found"; exit 1 }

$tog = $box.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
$tog.Toggle()
Write-Output "toggled '$Name' -> $($box.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState)"

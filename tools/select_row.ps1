param([int]$ProcId, [string]$RowName = "Row 1")

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($win -eq $null) { Write-Output "window not found"; exit 1 }

$rowCond = New-Object System.Windows.Automation.AndCondition(
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::ListItem)),
    (New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $RowName)))
$row = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $rowCond)
if ($row -eq $null) { Write-Output "row not found"; exit 1 }

$sel = $row.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
$sel.Select()
Write-Output "selected $RowName"

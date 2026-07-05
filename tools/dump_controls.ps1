param([int]$ProcId, [string]$Filter = "")

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
if ($wins.Count -eq 0) { Write-Output "window not found"; exit 1 }

foreach ($win in $wins) {
    Write-Output ("== window: '{0}'" -f $win.Current.Name)
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($el in $all) {
        $name = $el.Current.Name
        $type = $el.Current.ControlType.ProgrammaticName
        if ($Filter -eq "" -or $name -like "*$Filter*") {
            Write-Output ("{0} | '{1}'" -f $type, $name)
        }
    }
}

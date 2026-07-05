param([int]$ProcId)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
if ($win -eq $null) { Write-Output "window not found"; exit 1 }

$comboCond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::ComboBox)
$combos = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $comboCond)
Write-Output "combo boxes: $($combos.Count)"
$i = 0
foreach ($c in $combos) {
    $cur = $c.Current
    $val = ""
    try {
        $vp = $c.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        $val = $vp.Current.Value
    } catch {}
    Write-Output "[$i] name='$($cur.Name)' value='$val' rect=$($cur.BoundingRectangle)"
    $i++
}

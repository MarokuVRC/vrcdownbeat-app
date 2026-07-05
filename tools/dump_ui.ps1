param([int]$ProcId)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $ProcId)
$wins = $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)
Write-Output "top-level windows: $($wins.Count)"
foreach ($win in $wins) {
    Write-Output "=== '$($win.Current.Name)' ==="
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) {
        $c = $e.Current
        $t = $c.ControlType.ProgrammaticName -replace 'ControlType\.', ''
        if ($c.Name -and $c.Name.Trim()) { Write-Output "[$t] $($c.Name)" }
    }
}

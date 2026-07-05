param([int]$X, [int]$Y)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase

$pt = New-Object System.Windows.Point($X, $Y)
$el = [System.Windows.Automation.AutomationElement]::FromPoint($pt)
if ($el -eq $null) { Write-Output "nothing"; exit 1 }

# Walk up the tree printing each ancestor.
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
while ($el -ne $null) {
    Write-Output ("{0} | '{1}' | pid {2}" -f $el.Current.ControlType.ProgrammaticName, $el.Current.Name, $el.Current.ProcessId)
    $el = $walker.GetParent($el)
}

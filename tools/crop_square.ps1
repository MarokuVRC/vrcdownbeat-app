param([string]$InFile, [string]$OutFile)

Add-Type -AssemblyName System.Drawing

$src = [System.Drawing.Image]::FromFile($InFile)
$side = [Math]::Min($src.Width, $src.Height)
$x = [int](($src.Width - $side) / 2)
$y = [int](($src.Height - $side) / 2)

$bmp = New-Object System.Drawing.Bitmap($side, $side)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $side, $side)),
             (New-Object System.Drawing.Rectangle($x, $y, $side, $side)),
             [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $src.Dispose()
Write-Output "saved $OutFile ($side x $side)"

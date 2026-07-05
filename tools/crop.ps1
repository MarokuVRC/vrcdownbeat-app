param([string]$InFile, [int]$X, [int]$Y, [int]$W, [int]$H, [string]$OutFile)
Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile($InFile)
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $W, $H)),
             (New-Object System.Drawing.Rectangle($X, $Y, $W, $H)),
             [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save($OutFile, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $src.Dispose()
Write-Output "saved $OutFile"

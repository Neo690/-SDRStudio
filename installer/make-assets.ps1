# Genere les visuels de l'installateur (icone + images de l'assistant) aux
# couleurs de l'application : fond presque noir, accent turquoise, vague de
# spectre. Rien n'est versionne : les fichiers sont recrees a chaque build.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File make-assets.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$assets = Join-Path $here 'assets'
if (-not (Test-Path $assets)) { New-Item -ItemType Directory -Path $assets | Out-Null }

# Palette identique a android/app/src/main/res/values/colors.xml
$bgTop   = [System.Drawing.Color]::FromArgb(255, 11, 15, 20)
$bgBot   = [System.Drawing.Color]::FromArgb(255, 17, 28, 38)
$surface = [System.Drawing.Color]::FromArgb(255, 26, 36, 47)
$edge    = [System.Drawing.Color]::FromArgb(255, 38, 50, 63)
$accent  = [System.Drawing.Color]::FromArgb(255, 53, 208, 186)
$accent2 = [System.Drawing.Color]::FromArgb(255, 24, 128, 122)
$text    = [System.Drawing.Color]::FromArgb(255, 232, 239, 247)
$dim     = [System.Drawing.Color]::FromArgb(255, 143, 163, 184)

function New-Canvas([int]$width, [int]$height) {
    $bmp = New-Object System.Drawing.Bitmap($width, $height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    return @($bmp, $g)
}

function Add-Gradient($g, [int]$width, [int]$height) {
    $rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $bgTop, $bgBot, 90.0)
    $g.FillRectangle($brush, $rect)
    $brush.Dispose()
}

function Get-RoundPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# Une sinusoide sous enveloppe : la meme signature visuelle que le waterfall.
function Add-Wave($g, [single]$cx, [single]$cy, [single]$width, [single]$amp,
                  [single]$cycles, [System.Drawing.Color]$colour, [single]$thick) {
    $pen = New-Object System.Drawing.Pen($colour, $thick)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $points = New-Object 'System.Collections.Generic.List[System.Drawing.PointF]'
    $steps = 160
    for ($i = 0; $i -le $steps; $i++) {
        $t = $i / $steps
        $x = $cx - $width / 2 + $width * $t
        $envelope = [math]::Sin([math]::PI * $t)
        $y = $cy - [math]::Sin($t * [math]::PI * 2 * $cycles) * $amp * $envelope
        $points.Add((New-Object System.Drawing.PointF([single]$x, [single]$y)))
    }
    if ($points.Count -ge 2) { $g.DrawLines($pen, $points.ToArray()) }
    $pen.Dispose()
}

function Save-Flat($bmp, [string]$path) {
    # 24 bits : les BMP de l'assistant Inno n'ont pas besoin d'alpha.
    $flat = New-Object System.Drawing.Bitmap($bmp.Width, $bmp.Height,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($flat)
    $g.DrawImage($bmp, 0, 0, $bmp.Width, $bmp.Height)
    $g.Dispose()
    $flat.Save($path, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $flat.Dispose()
}

# --------------------------------------------------------------- icone 256 px
$iconBmp = New-Object System.Drawing.Bitmap(256, 256,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($iconBmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)

$body = Get-RoundPath 10 10 236 236 54
$fillRect = New-Object System.Drawing.Rectangle(10, 10, 236, 236)
$fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush($fillRect, $surface, $bgTop, 45.0)
$g.FillPath($fill, $body)
$fill.Dispose()
$border = New-Object System.Drawing.Pen($edge, 6)
$g.DrawPath($border, $body)
$border.Dispose()

Add-Wave $g 128 124 176 52 1.5 $accent2 14
Add-Wave $g 128 124 176 30 2.5 $accent 7
$dot = New-Object System.Drawing.SolidBrush($accent)
$g.FillEllipse($dot, 118, 196, 20, 20)
$dot.Dispose()
$body.Dispose()
$g.Dispose()

# ICO mono-image, entree PNG (Vista et suivants savent la lire).
$stream = New-Object System.IO.MemoryStream
$iconBmp.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
$png = $stream.ToArray()
$stream.Dispose()
$icoPath = Join-Path $assets 'icon.ico'
$file = [System.IO.File]::Create($icoPath)
$w = New-Object System.IO.BinaryWriter($file)
$w.Write([UInt16]0)                 # reserve
$w.Write([UInt16]1)                 # type icone
$w.Write([UInt16]1)                 # 1 image
$w.Write([Byte]0)                   # largeur 0 = 256
$w.Write([Byte]0)                   # hauteur 0 = 256
$w.Write([Byte]0)                   # palette
$w.Write([Byte]0)                   # reserve
$w.Write([UInt16]1)                 # plans
$w.Write([UInt16]32)                # bits par pixel
$w.Write([UInt32]$png.Length)
$w.Write([UInt32]22)                # offset des donnees
$w.Write($png)
$w.Dispose()
$file.Dispose()
$iconBmp.Dispose()
Write-Host ("  icon.ico        {0} x {0}, {1} octets" -f 256, (Get-Item $icoPath).Length)

# ------------------------------------------------- image de gauche (moderne)
# 164x314 : taille historique de l'assistant ; WizardImageStretch laisse Inno
# l'agrandir proprement aux DPI eleves.
$big = New-Object System.Drawing.Bitmap(164, 314)
$g = [System.Drawing.Graphics]::FromImage($big)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
Add-Gradient $g 164 314

# halo discret derriere la vague
$halo = New-Object System.Drawing.Drawing2D.GraphicsPath
$halo.AddEllipse(12, 26, 140, 140)
$haloBrush = New-Object System.Drawing.Drawing2D.PathGradientBrush($halo)
$haloBrush.CenterColor = [System.Drawing.Color]::FromArgb(60, 53, 208, 186)
$haloBrush.SurroundColors = @([System.Drawing.Color]::FromArgb(0, 11, 15, 20))
$g.FillPath($haloBrush, $halo)
$haloBrush.Dispose()
$halo.Dispose()

Add-Wave $g 82 96 140 40 1.5 $accent2 4
Add-Wave $g 82 96 140 24 2.5 $accent 2

$center = New-Object System.Drawing.StringFormat
$center.Alignment = [System.Drawing.StringAlignment]::Center
$center.LineAlignment = [System.Drawing.StringAlignment]::Near

$fTitle = New-Object System.Drawing.Font('Segoe UI', [single]30,
    [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$bTitle = New-Object System.Drawing.SolidBrush($text)
$g.DrawString('SDR', $fTitle, $bTitle, [single]82, [single]170, $center)

$fWord = New-Object System.Drawing.Font('Segoe UI', [single]12,
    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$bAccent = New-Object System.Drawing.SolidBrush($accent)
$g.DrawString('S T U D I O', $fWord, $bAccent, [single]82, [single]208, $center)

$fSub = New-Object System.Drawing.Font('Segoe UI', [single]11,
    [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$bDim = New-Object System.Drawing.SolidBrush($dim)
$g.DrawString('console RTL-SDR', $fSub, $bDim, [single]82, [single]232, $center)
$g.DrawString('coeur DSP natif', $fSub, $bDim, [single]82, [single]248, $center)

$line = New-Object System.Drawing.Pen($accent2, 2)
$g.DrawLine($line, 52, 286, 112, 286)
$line.Dispose()

$g.Dispose()
Save-Flat $big (Join-Path $assets 'setup-big.bmp')
$big.Dispose()
Write-Host "  setup-big.bmp   164x314"

# -------------------------------------------------- image du coin (55 x 55)
$small = New-Object System.Drawing.Bitmap(55, 55)
$g = [System.Drawing.Graphics]::FromImage($small)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
Add-Gradient $g 55 55
$body = Get-RoundPath 6 6 43 43 14
$fillRect = New-Object System.Drawing.Rectangle(6, 6, 43, 43)
$fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush($fillRect, $surface, $bgTop, 45.0)
$g.FillPath($fill, $body)
$fill.Dispose()
$border = New-Object System.Drawing.Pen($edge, 2)
$g.DrawPath($border, $body)
$border.Dispose()
$body.Dispose()
Add-Wave $g 27 27 34 10 1.5 $accent2 3
Add-Wave $g 27 27 34 6 2.5 $accent 1.5
$g.Dispose()
Save-Flat $small (Join-Path $assets 'setup-small.bmp')
$small.Dispose()
Write-Host "  setup-small.bmp 55x55"

Write-Host "Visuels generes dans $assets"

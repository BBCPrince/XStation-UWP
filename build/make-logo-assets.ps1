$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$assetDir = Join-Path $root 'source\Assets'
$master = [System.Drawing.Image]::FromFile((Join-Path $root 'docs\branding\xstation-xs-two-tone.png'))

function Write-Logo([string]$Name, [int]$Width, [int]$Height) {
    $bitmap = [System.Drawing.Bitmap]::new($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::FromArgb(255, 8, 8, 8))
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $side = [Math]::Min($Width, $Height)
        $rect = [System.Drawing.Rectangle]::new([int](($Width - $side) / 2), [int](($Height - $side) / 2), $side, $side)
        $graphics.DrawImage($master, $rect)
        $bitmap.Save((Join-Path $assetDir "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        # New resource names prevent the Xbox shell from reusing its old icon cache.
        $bitmap.Save((Join-Path $assetDir "XS-$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

try {
    Write-Logo 'Square150x150Logo' 150 150
    Write-Logo 'Square310x310Logo' 310 310
    Write-Logo 'Square44x44Logo' 44 44
    Write-Logo 'StoreLogo' 50 50
    foreach ($size in @(16, 24, 32, 48, 64, 256)) {
        Write-Logo "Square44x44Logo.targetsize-${size}_altform-unplated" $size $size
    }
    Write-Logo 'Wide310x150Logo' 310 150
    Write-Output 'Generated 11 XS app-icon sizes plus compatibility filenames. Existing splash and manifest preserved.'
} finally {
    $master.Dispose()
}

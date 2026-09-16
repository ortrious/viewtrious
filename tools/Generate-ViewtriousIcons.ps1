Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $root 'assets\icon_sources'
$assetDirectory = Join-Path $root 'assets'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function Get-PngBytes([System.Drawing.Image]$source, [int]$size) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppPArgb)
    try {
        $innerSize = $size - 2
        if ($innerSize -le 0) { throw "Icon frame leaves no drawable area." }
        # The master art's circular contour reaches its own canvas edge. Put transparent
        # overscan around it before downsampling so the resampler preserves the contour
        # instead of clamping its edge pixels into a visibly flat cut.
        $sourceScale = [double][Math]::Max($source.Width, $source.Height) / $innerSize
        # Preserve roughly 1.75 destination pixels of transparent sampling margin per
        # edge. This avoids leaving faint contour pixels on a frame boundary without
        # returning to the previous multi-pixel frame-padding policy.
        $sourcePadding = [int][Math]::Ceiling($sourceScale * 1.85)
        $sourceCanvas = [System.Drawing.Bitmap]::new($source.Width + 2 * $sourcePadding, $source.Height + 2 * $sourcePadding, [System.Drawing.Imaging.PixelFormat]::Format32bppPArgb)
        try {
            $sourceGraphics = [System.Drawing.Graphics]::FromImage($sourceCanvas)
            try {
                $sourceGraphics.Clear([System.Drawing.Color]::Transparent)
                $sourceGraphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
                $sourceGraphics.DrawImage($source, [System.Drawing.Rectangle]::new($sourcePadding, $sourcePadding, $source.Width, $source.Height))
            } finally {
                $sourceGraphics.Dispose()
            }
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.DrawImage($sourceCanvas, [System.Drawing.Rectangle]::new(0, 0, $size, $size))
            } finally {
                $graphics.Dispose()
            }
        } finally {
            $sourceCanvas.Dispose()
        }
        $stream = [System.IO.MemoryStream]::new()
        try {
            $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            return ,$stream.ToArray()
        } finally {
            $stream.Dispose()
        }
    } finally {
        $bitmap.Dispose()
    }
}

function New-Icon([string]$sourceName, [string]$outputName) {
    $source = [System.Drawing.Image]::FromFile((Join-Path $sourceDirectory $sourceName))
    try {
        $frames = foreach ($size in $sizes) {
            [pscustomobject]@{
                Size = $size
                Bytes = Get-PngBytes $source $size
            }
        }
        $stream = [System.IO.MemoryStream]::new()
        $writer = [System.IO.BinaryWriter]::new($stream)
        try {
            $writer.Write([UInt16]0)
            $writer.Write([UInt16]1)
            $writer.Write([UInt16]$frames.Count)
            $offset = 6 + 16 * $frames.Count
            foreach ($frame in $frames) {
                $writer.Write([byte]$(if ($frame.Size -eq 256) { 0 } else { $frame.Size }))
                $writer.Write([byte]$(if ($frame.Size -eq 256) { 0 } else { $frame.Size }))
                $writer.Write([byte]0)
                $writer.Write([byte]0)
                $writer.Write([UInt16]1)
                $writer.Write([UInt16]32)
                $writer.Write([UInt32]$frame.Bytes.Length)
                $writer.Write([UInt32]$offset)
                $offset += $frame.Bytes.Length
            }
            foreach ($frame in $frames) { $writer.Write($frame.Bytes) }
            [System.IO.File]::WriteAllBytes((Join-Path $assetDirectory $outputName), $stream.ToArray())
        } finally {
            $writer.Dispose()
            $stream.Dispose()
        }
    } finally {
        $source.Dispose()
    }
}

New-Icon 'icon_1024.png' 'Viewtrious.ico'
New-Icon 'icon_play_1024.png' 'ViewtriousVideo.ico'
New-Icon 'icon_3d_1024.png' 'Viewtrious3D.ico'

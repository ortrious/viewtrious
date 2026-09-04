Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $root 'new_icons'
$assetDirectory = Join-Path $root 'assets'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function Get-PngBytes([System.Drawing.Image]$source, [int]$size) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([System.Drawing.Color]::Transparent)
            $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.DrawImage($source, [System.Drawing.Rectangle]::new(0, 0, $size, $size))
        } finally {
            $graphics.Dispose()
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

function New-Icon([string]$smallSourceName, [string]$largeSourceName, [string]$outputName) {
    $small = [System.Drawing.Image]::FromFile((Join-Path $sourceDirectory $smallSourceName))
    $large = [System.Drawing.Image]::FromFile((Join-Path $sourceDirectory $largeSourceName))
    try {
        $frames = foreach ($size in $sizes) {
            [pscustomobject]@{
                Size = $size
                Bytes = Get-PngBytes $(if ($size -le 32) { $small } else { $large }) $size
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
        $small.Dispose()
        $large.Dispose()
    }
}

New-Icon 'icon_32.png' 'icon_1024.png' 'Viewtrious.ico'
New-Icon 'icon_play_32.png' 'icon_play_1024.png' 'ViewtriousVideo.ico'
New-Icon 'icon_3d_32_new.png' 'icon_3d_1024new.png' 'Viewtrious3D.ico'

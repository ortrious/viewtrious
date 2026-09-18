[CmdletBinding()]
param(
    [string]$ReleaseDirectory,
    [string]$IdentityName = 'Viewtrious.Development',
    [string]$Publisher = 'CN=Viewtrious Development',
    [string]$PublisherDisplayName = 'Ortrious (Development)',
    [string]$DisplayName = 'viewtrious',
    [switch]$SignForLocalTest,
    [string]$CertificatePath,
    [Security.SecureString]$CertificatePassword
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $ReleaseDirectory) { $ReleaseDirectory = Join-Path $root 'out\release' }
$release = (Resolve-Path $ReleaseDirectory).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw "Visual Studio locator was not found: $vswhere" }
$visualStudio = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $visualStudio) { throw 'Visual Studio C++ Build Tools were not found.' }
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) { throw "Visual Studio environment script was not found: $vcvars" }
$developerEnvironment = & cmd.exe /d /s /c "`"$vcvars`" >nul && set"
$developerPath = $developerEnvironment |
    Where-Object { $_ -match '^(?i:PATH)=(.*)$' } |
    Sort-Object Length -Descending |
    Select-Object -First 1
foreach ($line in $developerEnvironment) {
    if ($line -match '^([^=]+)=(.*)$' -and $matches[1] -ine 'PATH') {
        Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
    }
}
if ($developerPath -match '^(?i:PATH)=(.*)$') { Set-Item -Path 'Env:Path' -Value $matches[1] }

function Find-WindowsSdkTool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
    if ($command) { return $command }
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $sdkBin -PathType Container) {
        $candidate = Get-ChildItem -LiteralPath $sdkBin -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "x64\$Name" } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            Select-Object -First 1
        if ($candidate) { return $candidate }
    }
    throw "$Name was not found in PATH or the Windows SDK."
}

$cmake = Get-Command cmake.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source
& $cmake --build $release --config Release --target Viewtrious ViewtriousStlThumbnail
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }

$viewer = Join-Path $release 'viewtrious.exe'
$thumbnailProvider = Join-Path $release 'ViewtriousStlThumbnail.dll'
foreach ($artifact in @($viewer, $thumbnailProvider)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) { throw "Missing Release artifact: $artifact" }
}

$version = ([regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(Viewtrious VERSION ([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)')).Groups[1].Value
if (-not $version) { throw 'Could not determine the four-part Viewtrious version from CMakeLists.txt.' }

$outputDirectory = Join-Path $root 'out\msix'
$stagingDirectory = Join-Path $outputDirectory 'staging'
if (Test-Path -LiteralPath $stagingDirectory) { Remove-Item -LiteralPath $stagingDirectory -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'Assets') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'licenses') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stagingDirectory 'shellextensions') -Force | Out-Null

Copy-Item -LiteralPath $viewer -Destination (Join-Path $stagingDirectory 'viewtrious.exe')
Copy-Item -LiteralPath $thumbnailProvider -Destination (Join-Path $stagingDirectory 'shellextensions\ViewtriousStlThumbnail.dll')
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $stagingDirectory 'licenses\viewtrious.txt')
Copy-Item -LiteralPath (Join-Path $root 'NOTICE') -Destination (Join-Path $stagingDirectory 'licenses\notice.txt')
Copy-Item -LiteralPath (Join-Path $root 'app\licenses\miniz.txt') -Destination (Join-Path $stagingDirectory 'licenses\miniz.txt')

Add-Type -AssemblyName System.Drawing
function Write-SquarePng([string]$Source, [string]$Destination, [int]$Size) {
    $sourceImage = [System.Drawing.Image]::FromFile($Source)
    try {
        $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $bitmap.SetResolution(96.0, 96.0)
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
                $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.DrawImage($sourceImage, 0, 0, $Size, $Size)
            }
            finally { $graphics.Dispose() }
            $bitmap.Save($Destination, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        finally { $bitmap.Dispose() }
    }
    finally { $sourceImage.Dispose() }
}

$assets = Join-Path $stagingDirectory 'Assets'
$mainIcon = Join-Path $root 'assets\icon_sources\icon_1024.png'
Write-SquarePng $mainIcon (Join-Path $assets 'StoreLogo.png') 50
Write-SquarePng $mainIcon (Join-Path $assets 'Square44x44Logo.png') 44
Write-SquarePng $mainIcon (Join-Path $assets 'Square150x150Logo.png') 150
$appListTargetSizes = @(16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256)
foreach ($size in $appListTargetSizes) {
    foreach ($alternateForm in @('', '_altform-unplated', '_altform-lightunplated')) {
        $fileName = "Square44x44Logo.targetsize-$size$alternateForm.png"
        Write-SquarePng $mainIcon (Join-Path $assets $fileName) $size
    }
}
Write-SquarePng (Join-Path $root 'assets\icon_sources\icon_play_1024.png') (Join-Path $assets 'Square44x44VideoLogo.png') 44
Write-SquarePng (Join-Path $root 'assets\icon_sources\icon_3d_1024.png') (Join-Path $assets 'Square44x44ModelLogo.png') 44

function Escape-Xml([string]$Value) { return [System.Security.SecurityElement]::Escape($Value) }
$manifest = Get-Content -LiteralPath (Join-Path $root 'installer\msix\AppxManifest.xml.in') -Raw
$manifest = $manifest.Replace('@PACKAGE_IDENTITY_NAME@', (Escape-Xml $IdentityName))
$manifest = $manifest.Replace('@PACKAGE_PUBLISHER@', (Escape-Xml $Publisher))
$manifest = $manifest.Replace('@PACKAGE_VERSION@', $version)
$manifest = $manifest.Replace('@PACKAGE_DISPLAY_NAME@', (Escape-Xml $DisplayName))
$manifest = $manifest.Replace('@PACKAGE_PUBLISHER_DISPLAY_NAME@', (Escape-Xml $PublisherDisplayName))
if ($manifest -match '@PACKAGE_[A-Z_]+@') { throw 'The generated AppxManifest.xml contains an unresolved placeholder.' }
$manifestPath = Join-Path $stagingDirectory 'AppxManifest.xml'
[System.IO.File]::WriteAllText($manifestPath, $manifest, [System.Text.UTF8Encoding]::new($false))

$makePri = Find-WindowsSdkTool 'MakePri.exe'
$priConfig = Join-Path $outputDirectory 'priconfig.xml'
& $makePri createconfig /cf $priConfig /dq en-US /o
if ($LASTEXITCODE -ne 0) { throw "MakePri configuration generation failed with exit code $LASTEXITCODE." }
& $makePri new /pr $stagingDirectory /cf $priConfig /mn $manifestPath /of (Join-Path $stagingDirectory 'resources.pri') /o
if ($LASTEXITCODE -ne 0) { throw "MakePri resource indexing failed with exit code $LASTEXITCODE." }

$makeAppx = Find-WindowsSdkTool 'MakeAppx.exe'
$unsignedPackage = Join-Path $outputDirectory "viewtrious-$version-x64.msix"
& $makeAppx pack /v /o /h SHA256 /d $stagingDirectory /p $unsignedPackage
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed with exit code $LASTEXITCODE." }

Write-Host "Unsigned MSIX: $unsignedPackage ($((Get-Item -LiteralPath $unsignedPackage).Length) bytes)"
if ($SignForLocalTest) {
    if (-not $CertificatePath) { throw '-CertificatePath is required with -SignForLocalTest.' }
    $certificate = (Resolve-Path $CertificatePath).Path
    $signTool = Find-WindowsSdkTool 'SignTool.exe'
    $signedPackage = Join-Path $outputDirectory "viewtrious-$version-x64-localtest.msix"
    Copy-Item -LiteralPath $unsignedPackage -Destination $signedPackage -Force
    $arguments = @('sign', '/fd', 'SHA256', '/a', '/f', $certificate)
    $plainPassword = $null
    if ($CertificatePassword) {
        $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($CertificatePassword)
        try { $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer) }
        $arguments += @('/p', $plainPassword)
    }
    try { & $signTool @arguments $signedPackage }
    finally { $plainPassword = $null }
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed with exit code $LASTEXITCODE." }
    Write-Host "Signed local-test MSIX: $signedPackage ($((Get-Item -LiteralPath $signedPackage).Length) bytes)"
}

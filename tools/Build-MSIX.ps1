[CmdletBinding()]
param(
    [string]$ReleaseDirectory,
    [switch]$Store,
    [string]$ThreeDxWareSdkRoot,
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
$storeIdentityName = 'ortrious.viewtrious'
$storePublisher = 'CN=3A400F6A-D8BA-4784-84CE-0351CAC0C7F3'
$storePublisherDisplayName = 'ortrious'
$mode = if ($Store) { 'store' } else { 'development' }
if ($Store) {
    foreach ($parameter in @('IdentityName', 'Publisher', 'PublisherDisplayName', 'DisplayName')) {
        if ($PSBoundParameters.ContainsKey($parameter)) {
            throw "-$parameter cannot override the Partner Center identity in Store mode."
        }
    }
    $IdentityName = $storeIdentityName
    $Publisher = $storePublisher
    $PublisherDisplayName = $storePublisherDisplayName
    $DisplayName = 'viewtrious'
}
if (-not $ReleaseDirectory) {
    $ReleaseDirectory = if ($Store) { Join-Path $root 'out\build\msix-store' } else { Join-Path $root 'out\release' }
}

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
if ($Store) {
    if (-not $ThreeDxWareSdkRoot) {
        $seedCache = Join-Path $root 'out\release\CMakeCache.txt'
        if (Test-Path -LiteralPath $seedCache -PathType Leaf) {
            $match = [regex]::Match((Get-Content -LiteralPath $seedCache -Raw), '(?m)^VIEWTRIOUS_3DXWARE_SDK_ROOT:PATH=(.+)$')
            if ($match.Success) { $ThreeDxWareSdkRoot = $match.Groups[1].Value.Trim() }
        }
    }
    if (-not $ThreeDxWareSdkRoot) {
        throw 'Store configuration requires -ThreeDxWareSdkRoot when out\release\CMakeCache.txt cannot supply it.'
    }
    & $cmake -S $root -B $ReleaseDirectory -G Ninja '-DCMAKE_BUILD_TYPE=Release' `
        "-DVIEWTRIOUS_3DXWARE_SDK_ROOT=$ThreeDxWareSdkRoot" `
        '-DVIEWTRIOUS_STORE_BUILD=ON' '-DVIEWTRIOUS_ENABLE_UPDATE_CHECKS=OFF' `
        '-DVIEWTRIOUS_BUILD_AI_ADDON=OFF' '-DVIEWTRIOUS_SHOW_DEV_VERSION_BADGE=OFF'
    if ($LASTEXITCODE -ne 0) { throw "Store Release configuration failed with exit code $LASTEXITCODE." }
}
$release = (Resolve-Path $ReleaseDirectory).Path
& $cmake --build $release --config Release --target Viewtrious ViewtriousStlThumbnail
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }

if ($Store) {
    $cache = Get-Content -LiteralPath (Join-Path $release 'CMakeCache.txt') -Raw
    if ($cache -notmatch '(?m)^VIEWTRIOUS_STORE_BUILD:BOOL=ON\r?$' -or
        $cache -notmatch '(?m)^VIEWTRIOUS_ENABLE_UPDATE_CHECKS:BOOL=OFF\r?$') {
        throw 'Store build configuration did not enforce VIEWTRIOUS_STORE_BUILD=ON and VIEWTRIOUS_ENABLE_UPDATE_CHECKS=OFF.'
    }
}

$viewer = Join-Path $release 'viewtrious.exe'
$thumbnailProvider = Join-Path $release 'ViewtriousStlThumbnail.dll'
foreach ($artifact in @($viewer, $thumbnailProvider)) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) { throw "Missing Release artifact: $artifact" }
}

$version = ([regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(Viewtrious VERSION ([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)')).Groups[1].Value
if (-not $version) { throw 'Could not determine the four-part Viewtrious version from CMakeLists.txt.' }

$outputDirectory = Join-Path $root "out\msix\$mode"
$workDirectory = Join-Path $root "out\msix\.work-$mode"
$stagingDirectory = Join-Path $workDirectory 'staging'
if (Test-Path -LiteralPath $stagingDirectory) { Remove-Item -LiteralPath $stagingDirectory -Recurse -Force }
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
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
Copy-Item -LiteralPath (Join-Path $root 'assets\Viewtrious.ico') -Destination $assets
Copy-Item -LiteralPath (Join-Path $root 'assets\ViewtriousVideo.ico') -Destination $assets
Copy-Item -LiteralPath (Join-Path $root 'assets\Viewtrious3D.ico') -Destination $assets
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

[xml]$manifestXml = $manifest
if ($manifestXml.Package.Identity.Name -cne $IdentityName -or
    $manifestXml.Package.Identity.Publisher -cne $Publisher -or
    $manifestXml.Package.Identity.Version -cne $version -or
    $manifestXml.Package.Identity.ProcessorArchitecture -cne 'x64' -or
    $manifestXml.Package.Properties.DisplayName -cne $DisplayName -or
    $manifestXml.Package.Properties.PublisherDisplayName -cne $PublisherDisplayName) {
    throw 'Generated AppxManifest.xml identity, version, architecture, or display metadata is incorrect.'
}
foreach ($requiredManifestText in @(
    'EntryPoint="Windows.FullTrustApplication"',
    'D812B4F2-B141-4A0D-9A4F-574DDB2975B2',
    '<desktop2:ThumbnailHandler',
    'Name="Microsoft.VCLibs.140.00.UWPDesktop"',
    'Assets\Viewtrious.ico',
    'Assets\ViewtriousVideo.ico',
    'Assets\Viewtrious3D.ico',
    '<uap:FileType>.stl</uap:FileType>',
    '<uap:FileType>.3mf</uap:FileType>'
)) {
    if (-not $manifest.Contains($requiredManifestText)) { throw "Generated manifest is missing required declaration: $requiredManifestText" }
}
if ($Store -and ($manifest.Contains('Viewtrious.Development') -or
    $manifest.Contains('CN=Viewtrious Development') -or
    $manifest.Contains('Ortrious (Development)'))) {
    throw 'Generated Store manifest contains a development identity value.'
}

$makePri = Find-WindowsSdkTool 'MakePri.exe'
$priConfig = Join-Path $workDirectory 'priconfig.xml'
& $makePri createconfig /cf $priConfig /dq en-US /o
if ($LASTEXITCODE -ne 0) { throw "MakePri configuration generation failed with exit code $LASTEXITCODE." }
& $makePri new /pr $stagingDirectory /cf $priConfig /mn $manifestPath /of (Join-Path $stagingDirectory 'resources.pri') /o
if ($LASTEXITCODE -ne 0) { throw "MakePri resource indexing failed with exit code $LASTEXITCODE." }

$makeAppx = Find-WindowsSdkTool 'MakeAppx.exe'
$packageStem = "viewtrious-$version-x64-$mode"
$unsignedPackage = Join-Path $outputDirectory "$packageStem.msix"
& $makeAppx pack /v /o /h SHA256 /d $stagingDirectory /p $unsignedPackage
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed with exit code $LASTEXITCODE." }

$inspectionDirectory = Join-Path $workDirectory 'inspection'
if (Test-Path -LiteralPath $inspectionDirectory) { Remove-Item -LiteralPath $inspectionDirectory -Recurse -Force }
& $makeAppx unpack /v /o /p $unsignedPackage /d $inspectionDirectory
if ($LASTEXITCODE -ne 0) { throw "MakeAppx package inspection failed with exit code $LASTEXITCODE." }
$allowedPackagePaths = @(
    '^\[Content_Types\]\.xml$',
    '^AppxBlockMap\.xml$',
    '^AppxManifest\.xml$',
    '^resources\.pri$',
    '^viewtrious\.exe$',
    '^licenses\\(viewtrious|notice|miniz)\.txt$',
    '^shellextensions\\ViewtriousStlThumbnail\.dll$',
    '^Assets\\(Viewtrious|ViewtriousVideo|Viewtrious3D)\.ico$',
    '^Assets\\StoreLogo\.png$',
    '^Assets\\Square(44x44|150x150)Logo\.png$',
    '^Assets\\Square44x44Logo\.targetsize-[0-9]+(_altform-(light)?unplated)?\.png$'
)
$packageFiles = Get-ChildItem -LiteralPath $inspectionDirectory -Recurse -File
foreach ($file in $packageFiles) {
    $relativePath = $file.FullName.Substring($inspectionDirectory.Length + 1)
    if (-not ($allowedPackagePaths | Where-Object { $relativePath -match $_ })) {
        throw "Unexpected file in MSIX payload: $relativePath"
    }
}
foreach ($requiredPackagePath in @('viewtrious.exe', 'shellextensions\ViewtriousStlThumbnail.dll', 'AppxManifest.xml', 'resources.pri')) {
    if (-not (Test-Path -LiteralPath (Join-Path $inspectionDirectory $requiredPackagePath) -PathType Leaf)) {
        throw "Required file is missing from MSIX payload: $requiredPackagePath"
    }
}
if ($Store) {
    $forbiddenStrings = @('Viewtrious.Development', 'CN=Viewtrious Development', 'Ortrious (Development)', 'viewtrious.com', '/update.json')
    foreach ($file in $packageFiles) {
        $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
        $ascii = [System.Text.Encoding]::UTF8.GetString($bytes)
        $unicode = [System.Text.Encoding]::Unicode.GetString($bytes)
        foreach ($forbidden in $forbiddenStrings) {
            if ($ascii.Contains($forbidden) -or $unicode.Contains($forbidden)) {
                throw "Store MSIX contains forbidden development/updater string '$forbidden' in $($file.Name)."
            }
        }
    }
}

Write-Host "Unsigned MSIX: $unsignedPackage ($((Get-Item -LiteralPath $unsignedPackage).Length) bytes)"
if ($SignForLocalTest) {
    if (-not $CertificatePath) { throw '-CertificatePath is required with -SignForLocalTest.' }
    $certificate = (Resolve-Path $CertificatePath).Path
    $plainPassword = $null
    if ($CertificatePassword) {
        $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($CertificatePassword)
        try { $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer) }
        finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer) }
    }
    try {
        $flags = [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
        $signingCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificate, $plainPassword, $flags)
        try {
            if ($signingCertificate.Subject -cne $Publisher) {
                throw "Certificate subject '$($signingCertificate.Subject)' does not exactly match manifest Publisher '$Publisher'."
            }
            if (-not $signingCertificate.HasPrivateKey) { throw 'The supplied PFX does not contain a private key.' }
            $now = Get-Date
            if ($now -lt $signingCertificate.NotBefore -or $now -gt $signingCertificate.NotAfter) {
                throw 'The supplied signing certificate is not currently valid.'
            }
            $codeSigningEku = $false
            foreach ($extension in $signingCertificate.Extensions) {
                if ($extension.Oid.Value -ne '2.5.29.37') { continue }
                $enhancedKeyUsage = [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]$extension
                foreach ($usage in $enhancedKeyUsage.EnhancedKeyUsages) {
                    if ($usage.Value -eq '1.3.6.1.5.5.7.3.3') { $codeSigningEku = $true }
                }
            }
            if (-not $codeSigningEku) { throw 'The supplied certificate does not permit code signing.' }
        }
        finally { $signingCertificate.Dispose() }

        $signTool = Find-WindowsSdkTool 'SignTool.exe'
        $signedPackage = Join-Path $outputDirectory "$packageStem-localtest.msix"
        Copy-Item -LiteralPath $unsignedPackage -Destination $signedPackage -Force
        $arguments = @('sign', '/fd', 'SHA256', '/a', '/f', $certificate)
        if ($CertificatePassword) { $arguments += @('/p', $plainPassword) }
        & $signTool @arguments $signedPackage
    }
    finally { $plainPassword = $null }
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed with exit code $LASTEXITCODE." }
    Write-Host "Signed local-test MSIX: $signedPackage ($((Get-Item -LiteralPath $signedPackage).Length) bytes)"
}

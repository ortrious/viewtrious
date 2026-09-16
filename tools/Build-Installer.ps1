[CmdletBinding()]
param(
    [string]$ReleaseDirectory
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
foreach ($line in (& cmd.exe /d /s /c "`"$vcvars`" >nul && set")) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}
$cmake = Get-Command cmake.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source
& $cmake --build $release --config Release --target Viewtrious ViewtriousStlThumbnail ViewtriousSetup
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }
$required = @('viewtrious.exe', 'ViewtriousStlThumbnail.dll')
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $release $name) -PathType Leaf)) {
        throw "Missing Release artifact: $(Join-Path $release $name). Build the configured Release tree first."
    }
}
$makeNsis = Get-Command makensis.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (-not $makeNsis) {
    $standard = 'C:\Program Files (x86)\NSIS\makensis.exe'
    if (Test-Path -LiteralPath $standard -PathType Leaf) { $makeNsis = $standard }
}
if (-not $makeNsis) { throw 'NSIS 3.x makensis.exe was not found on PATH or at C:\Program Files (x86)\NSIS\makensis.exe.' }
$version = ([regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(Viewtrious VERSION ([0-9.]+)')).Groups[1].Value
if (-not $version) { throw 'Could not determine the Viewtrious version from CMakeLists.txt.' }
$installerDirectory = Join-Path $root 'out\installer'
$internalDirectory = Join-Path $installerDirectory 'internal'
New-Item -ItemType Directory -Path $internalDirectory -Force | Out-Null
$coreInstaller = Join-Path $internalDirectory "viewtrious-core-$version.exe"
$publicInstaller = Join-Path $installerDirectory "viewtrious-setup-$version.exe"
Get-ChildItem -LiteralPath $internalDirectory -Filter 'viewtrious-core-*.exe' -File |
    Where-Object FullName -ne $coreInstaller |
    Remove-Item -Force
& $makeNsis "/DPRODUCT_VERSION=$version" "/DRELEASE_DIR=$release" "/DSOURCE_DIR=$root" "/DOUTPUT_FILE=$coreInstaller" (Join-Path $root 'installer\viewtrious.nsi')
if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }

$setupStub = Join-Path $release 'ViewtriousSetup.exe'
if (-not (Test-Path -LiteralPath $setupStub -PathType Leaf)) { throw "Missing setup stub: $setupStub" }
Get-ChildItem -LiteralPath $installerDirectory -Filter 'viewtrious-setup-*.exe' -File |
    Where-Object FullName -ne $publicInstaller |
    Remove-Item -Force

$payloadHash = [System.Security.Cryptography.SHA256]::Create()
try {
    $payloadStream = [System.IO.File]::OpenRead($coreInstaller)
    try { $hash = $payloadHash.ComputeHash($payloadStream) }
    finally { $payloadStream.Dispose() }
}
finally { $payloadHash.Dispose() }

$destination = $null
for ($attempt = 0; $attempt -lt 20 -and -not $destination; ++$attempt) {
    try {
        $destination = [System.IO.File]::Open($publicInstaller, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    }
    catch [System.IO.IOException] {
        if ($attempt -eq 19) { throw }
        Start-Sleep -Milliseconds 100
    }
}
try {
    $stubStream = [System.IO.File]::OpenRead($setupStub)
    try { $stubStream.CopyTo($destination) }
    finally { $stubStream.Dispose() }
    $payloadStream = [System.IO.File]::OpenRead($coreInstaller)
    try { $payloadStream.CopyTo($destination) }
    finally { $payloadStream.Dispose() }
    $writer = [System.IO.BinaryWriter]::new($destination, [System.Text.Encoding]::UTF8, $true)
    try {
        $magic = [System.Text.Encoding]::ASCII.GetBytes("VTRSETUPPAYLOAD`0")
        $writer.Write($magic)
        $writer.Write([uint32]1)
        $writer.Write([uint64](Get-Item -LiteralPath $coreInstaller).Length)
        $writer.Write($hash)
    }
    finally { $writer.Dispose() }
}
finally { $destination.Dispose() }

$metadata = (Get-Item -LiteralPath $publicInstaller).VersionInfo
if ($metadata.ProductVersion -ne $version) { throw "Unexpected setup ProductVersion '$($metadata.ProductVersion)'." }
Write-Host "Public installer: $publicInstaller ($((Get-Item -LiteralPath $publicInstaller).Length) bytes)"
Write-Host "Internal backend: $coreInstaller ($((Get-Item -LiteralPath $coreInstaller).Length) bytes)"

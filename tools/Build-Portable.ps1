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

& $cmake --build $release --config Release --target Viewtrious
if ($LASTEXITCODE -ne 0) { throw "Release build failed with exit code $LASTEXITCODE." }

$viewer = Join-Path $release 'viewtrious.exe'
if (-not (Test-Path -LiteralPath $viewer -PathType Leaf)) {
    throw "Missing Release artifact: $viewer"
}

$version = ([regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(Viewtrious VERSION ([0-9.]+)')).Groups[1].Value
if (-not $version) { throw 'Could not determine the Viewtrious version from CMakeLists.txt.' }

$portableRoot = Join-Path $root 'out\portable'
$packageName = "viewtrious-portable-$version"
$packageDirectory = Join-Path $portableRoot $packageName
$zipPath = Join-Path $portableRoot "$packageName.zip"
if (Test-Path -LiteralPath $packageDirectory) { Remove-Item -LiteralPath $packageDirectory -Recurse -Force }
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }

$licenses = Join-Path $packageDirectory 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath $viewer -Destination (Join-Path $packageDirectory 'viewtrious.exe')
[System.IO.File]::WriteAllBytes((Join-Path $packageDirectory 'viewtrious.portable'), [byte[]]@())
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $licenses 'viewtrious.txt')
Copy-Item -LiteralPath (Join-Path $root 'app\licenses\miniz.txt') -Destination (Join-Path $licenses 'miniz.txt')
Copy-Item -LiteralPath (Join-Path $root 'NOTICE') -Destination (Join-Path $licenses 'notice.txt')

Compress-Archive -LiteralPath $packageDirectory -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host "Portable package: $packageDirectory"
Write-Host "Portable ZIP: $zipPath ($((Get-Item -LiteralPath $zipPath).Length) bytes)"

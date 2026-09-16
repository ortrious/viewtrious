[CmdletBinding()]
param(
    [string]$ReleaseDirectory = (Join-Path $PSScriptRoot '..\out\release')
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$release = (Resolve-Path $ReleaseDirectory).Path
$required = @('Viewtrious.exe', 'ViewtriousStlThumbnail.dll')
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
New-Item -ItemType Directory -Path (Join-Path $root 'out\installer') -Force | Out-Null
& $makeNsis "/DPRODUCT_VERSION=$version" "/DRELEASE_DIR=$release" "/DSOURCE_DIR=$root" (Join-Path $root 'installer\viewtrious.nsi')
if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }

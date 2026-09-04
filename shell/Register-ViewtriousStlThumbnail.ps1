param([ValidateSet('Register','Unregister')][string]$Action='Register')

$clsid='{D812B4F2-B141-4A0D-9A4F-574DDB2975B2}'
$obsoleteViewtriousClsid='{6D3CF8C3-96CD-4E2E-B553-4AB90F097D1A}'
$thumbnail='{E357FCCD-A995-4576-B01F-234630154E96}'
$root='HKCU:\Software\Classes'
$extension="$root\.stl\shellex\$thumbnail"
$class="$root\CLSID\$clsid"
$obsoleteClass="$root\CLSID\$obsoleteViewtriousClsid"

function Get-RegisteredProvider([string]$Path) {
    $key=Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if($null -eq $key) { return $null }
    return $key.GetValue('')
}

$existing=Get-RegisteredProvider $extension
if($Action -eq 'Register') {
    $dll=Join-Path $PSScriptRoot 'ViewtriousStlThumbnail.dll'
    if(!(Test-Path -LiteralPath $dll)) { throw "Build the DLL before registering: $dll" }
    if($existing -and $existing -ne $clsid -and $existing -ne $obsoleteViewtriousClsid) { throw 'Another STL thumbnail provider is already registered. It was not changed.' }
    if($existing -eq $obsoleteViewtriousClsid) { Remove-Item -LiteralPath $obsoleteClass -Recurse -Force -ErrorAction SilentlyContinue }
    New-Item -Path $extension -Force | Out-Null
    Set-Item -Path $extension -Value $clsid
    New-Item -Path "$class\InprocServer32" -Force | Out-Null
    Set-Item -Path "$class\InprocServer32" -Value $dll
    New-ItemProperty -Path "$class\InprocServer32" -Name ThreadingModel -Value Both -PropertyType String -Force | Out-Null
} else {
    if($existing -eq $clsid) { Remove-Item -LiteralPath $extension -Recurse -Force }
    Remove-Item -LiteralPath $class -Recurse -Force -ErrorAction SilentlyContinue
}

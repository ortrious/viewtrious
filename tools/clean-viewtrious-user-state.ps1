<#
.SYNOPSIS
Preview or remove stale per-user Viewtrious development state.

.DESCRIPTION
Resets exact HKCU settings, application-registration keys, and Open With history
values belonging to the historical mediaView, FeatherView, and Viewtrious names.
Preview is the default; pass -Apply to delete the displayed items. This tool never
touches user images, HKLM, default-app choices, repositories, or shell caches.
#>
[CmdletBinding()]
param(
    [ValidateSet('All', 'mediaView', 'FeatherView', 'Viewtrious')]
    [string]$Target = 'All',
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$allIdentities = @('mediaView', 'FeatherView', 'Viewtrious')
$identities = if ($Target -eq 'All') { $allIdentities } else { @($Target) }
$executables = $identities | ForEach-Object { "$_.exe" }
$actions = [System.Collections.Generic.List[object]]::new()
$counts = @{ RegistryKeys = 0; RegistryValues = 0; Files = 0; Failed = 0 }
$removed = @{ RegistryKeys = 0; RegistryValues = 0; Files = 0 }

function Add-RegistryKeyAction([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        $script:actions.Add([pscustomobject]@{ Kind = 'RegistryKey'; Path = $Path; Name = $null; Value = $null })
    }
}

function Add-RegistryValueAction([string]$Path, [string]$Name, $Value) {
    $script:actions.Add([pscustomobject]@{ Kind = 'RegistryValue'; Path = $Path; Name = $Name; Value = $Value })
}

Write-Host 'Registry settings'
foreach ($identity in $identities) {
    Add-RegistryKeyAction "HKCU:\Software\$identity"
}

Write-Host 'Application registration'
foreach ($executable in $executables) {
    Add-RegistryKeyAction "HKCU:\Software\Classes\Applications\$executable"
}

Write-Host 'Open With history'
$fileExtsRoot = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts'
if (Test-Path -LiteralPath $fileExtsRoot) {
    foreach ($extensionKey in Get-ChildItem -LiteralPath $fileExtsRoot -ErrorAction SilentlyContinue) {
        $openWithList = Join-Path $extensionKey.PSPath 'OpenWithList'
        if (-not (Test-Path -LiteralPath $openWithList)) { continue }
        $properties = (Get-ItemProperty -LiteralPath $openWithList).PSObject.Properties
        $removedLetters = @()
        foreach ($property in $properties) {
            if ($property.Name -in 'PSPath', 'PSParentPath', 'PSChildName', 'PSDrive', 'PSProvider', 'MRUList') { continue }
            if ($property.Name -match '^[a-zA-Z]$' -and $executables -contains [string]$property.Value) {
                Add-RegistryValueAction $openWithList $property.Name $property.Value
                $removedLetters += $property.Name
            }
        }
        $mru = $properties | Where-Object Name -eq 'MRUList' | Select-Object -ExpandProperty Value -ErrorAction SilentlyContinue
        if ($removedLetters.Count -gt 0 -and $null -ne $mru) {
            $repaired = -join ($mru.ToCharArray() | Where-Object { $_ -notin $removedLetters })
            if ($repaired -ne $mru) { Add-RegistryValueAction $openWithList 'MRUList' $repaired }
        }
    }
}

Write-Host 'AppData files'
Write-Host '  Viewtrious stores runtime data under %LOCALAPPDATA%\viewtrious\data; this tool does not target that data.'

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $executables -contains "$($_.ProcessName).exe" }
if ($running) {
    $names = ($running | ForEach-Object { "$($_.ProcessName) (PID $($_.Id))" }) -join ', '
    Write-Host "Selected application process is running: $names"
    if ($Apply) {
        Write-Error 'Close the selected application before running with -Apply. Nothing was removed.'
        exit 1
    }
}

foreach ($action in $actions) {
    if ($action.Kind -eq 'RegistryKey') {
        Write-Host "Would remove registry key:`n  $($action.Path)"
        $counts.RegistryKeys++
    } else {
        $description = if ($action.Name -eq 'MRUList' -and [string]::IsNullOrEmpty($action.Value)) { 'remove empty MRUList' } else { "$($action.Name) = $($action.Value)" }
        Write-Host "Would remove registry value:`n  $($action.Path)`n  $description"
        $counts.RegistryValues++
    }
}

if (-not $Apply) {
    Write-Host "`nPreview complete."
    Write-Host "Registry keys: $($counts.RegistryKeys)"
    Write-Host "Registry values: $($counts.RegistryValues)"
    Write-Host 'Files/directories: 0'
    Write-Host 'Run again with -Apply to remove these items.'
    exit 0
}

foreach ($action in $actions) {
    try {
        if ($action.Kind -eq 'RegistryKey') {
            Remove-Item -LiteralPath $action.Path -Recurse -Force
            Write-Host "Removed registry key: $($action.Path)"
            $removed.RegistryKeys++
        } elseif ($action.Name -eq 'MRUList' -and [string]::IsNullOrEmpty($action.Value)) {
            Remove-ItemProperty -LiteralPath $action.Path -Name $action.Name -Force
            Write-Host "Removed empty MRUList: $($action.Path)"
            $removed.RegistryValues++
        } elseif ($action.Name -eq 'MRUList') {
            Set-ItemProperty -LiteralPath $action.Path -Name $action.Name -Value $action.Value
            Write-Host "Updated MRUList: $($action.Path) = $($action.Value)"
            $removed.RegistryValues++
        } else {
            Remove-ItemProperty -LiteralPath $action.Path -Name $action.Name -Force
            Write-Host "Removed registry value: $($action.Path)\$($action.Name)"
            $removed.RegistryValues++
        }
    } catch {
        $counts.Failed++
        Write-Warning "Skipped $($action.Path): $($_.Exception.Message)"
    }
}

Write-Host "`nCleanup complete."
Write-Host "Removed registry keys: $($removed.RegistryKeys)"
Write-Host "Removed registry values: $($removed.RegistryValues)"
Write-Host 'Removed files/directories: 0'
Write-Host "Skipped/failed: $($counts.Failed)"
if ($counts.Failed -gt 0) { exit 1 }

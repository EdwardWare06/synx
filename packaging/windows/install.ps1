# Installs this Synx package for the current user.
#   install.cmd                      copy to %LOCALAPPDATA%\Programs\Synx, add bin to PATH
#   install.cmd -Destination <dir>   copy to <dir>
#   install.cmd -NoPath              do not change PATH or SYNX_HOME
#   install.cmd -Uninstall           remove the installed copy and its PATH entry
# Run -Uninstall from a directory outside the installation.
param(
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs\Synx'),
    [switch]$NoPath,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$Destination = [System.IO.Path]::GetFullPath($Destination).TrimEnd('\')
$bin = Join-Path $Destination 'bin'
$items = @('bin', 'libexec', 'lib', 'std', 'examples', 'install.cmd', 'install.ps1', 'LICENSE-MIT', 'LICENSE-APACHE')

# A directory is replaced or removed only when it holds a Synx installation.
function Test-SynxInstall([string]$Path) {
    return (Test-Path -LiteralPath (Join-Path $Path 'libexec\synx.exe')) -and
           (Test-Path -LiteralPath (Join-Path $Path 'bin\synx.cmd'))
}

# The user PATH is read and written without expanding %VARIABLES%. Setting
# SYNX_HOME afterwards broadcasts the change to new processes.
function Get-UserPathEntries {
    $raw = (Get-Item -Path 'HKCU:\Environment').GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    return @($raw -split ';' | Where-Object { $_ -ne '' })
}
function Set-UserPathEntries([string[]]$Entries) {
    Set-ItemProperty -Path 'HKCU:\Environment' -Name 'Path' -Value ($Entries -join ';') -Type ExpandString
}
function Test-SameDirectory([string]$Left, [string]$Right) {
    return [string]::Equals($Left.TrimEnd('\'), $Right.TrimEnd('\'), [System.StringComparison]::OrdinalIgnoreCase)
}

if ($Uninstall) {
    if (Test-Path -LiteralPath $Destination) {
        if (-not (Test-SynxInstall $Destination)) { throw "$Destination does not hold a Synx installation." }
        Set-Location -LiteralPath $env:TEMP
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    if (-not $NoPath) {
        Set-UserPathEntries @(Get-UserPathEntries | Where-Object { -not (Test-SameDirectory $_ $bin) })
        [Environment]::SetEnvironmentVariable('SYNX_HOME', $null, 'User')
    }
    Write-Output "Removed Synx from $Destination."
    return
}

if (-not (Test-SynxInstall $PSScriptRoot)) { throw "$PSScriptRoot is not a complete Synx package." }
if (Test-SameDirectory $PSScriptRoot $Destination) { throw 'The package is already at the destination.' }
if (Test-Path -LiteralPath $Destination) {
    $existing = @(Get-ChildItem -LiteralPath $Destination -Force)
    if ($existing.Count -gt 0 -and -not (Test-SynxInstall $Destination)) {
        throw "$Destination is not empty and does not hold a Synx installation."
    }
    Remove-Item -LiteralPath $Destination -Recurse -Force
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
foreach ($item in $items) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $item) -Destination $Destination -Recurse -Force
}

if (-not $NoPath) {
    $entries = @(Get-UserPathEntries | Where-Object { -not (Test-SameDirectory $_ $bin) })
    Set-UserPathEntries (@($bin) + $entries)
    [Environment]::SetEnvironmentVariable('SYNX_HOME', $Destination, 'User')
    Write-Output "Installed Synx to $Destination and added $bin to your PATH."
    Write-Output 'Open a new terminal, then run: synx version'
} else {
    Write-Output "Installed Synx to $Destination. Run: `"$bin\synx.cmd`" version"
}

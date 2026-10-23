# Package steps for scripts\package-win.cmd, which loads the MSVC environment.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build-package-win'
$version = ([regex]::Match((Get-Content -Raw (Join-Path $root 'CMakeLists.txt')), 'project\(synx VERSION ([0-9.]+)')).Groups[1].Value
$name = "synx-$version-windows-x86_64"
$stageRoot = Join-Path $build 'stage'
$stage = Join-Path $stageRoot $name
$out = Join-Path $root 'dist\packages'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
}
function Invoke-WithoutToolchain([scriptblock]$Action) {
    $previousPath = $env:PATH
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        & $Action
    } finally { $env:PATH = $previousPath }
}

Invoke-Checked cmake @('-S', $root, '-B', $build, '-G', 'NMake Makefiles', '-DCMAKE_BUILD_TYPE=Release')
Invoke-Checked cmake @('--build', $build, '--target', 'synx_compiler', 'synx_rt', 'synx_rt_shared')
if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }
Invoke-Checked cmake @('--install', $build, '--prefix', $stage)

# The package holds only the launcher, compiler, runtime, standard library
# sources, the workbench example, the installer, and the license files.
$allowed = '^(bin/synx\.cmd|libexec/(synx\.exe|synx_rt\.dll)|lib/(synx_rt\.lib|synx_rt\.dll)|install\.cmd|install\.ps1|LICENSE-(MIT|APACHE)|std/.+\.snx|examples/workbench/[a-z_]+\.snx)$'
$unexpected = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    $_.FullName.Substring($stage.Length + 1).Replace('\', '/') } | Where-Object { $_ -notmatch $allowed })
if ($unexpected.Count -gt 0) { throw "unexpected package files: $($unexpected -join ', ')" }
# Imported CRT DLLs would make the package depend on a Visual C++ install.
# Inspect while the developer PATH still provides dumpbin.
foreach ($binary in (Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })) {
    $imports = & dumpbin /nologo /imports $binary.FullName 2>&1
    if ($LASTEXITCODE -ne 0) { throw "cannot inspect PE imports: $($binary.FullName)" }
    if (($imports -join "`n") -match '(?im)^\s*((vcruntime|msvcp|msvcr)[^\s]*|ucrtbased)\.dll\s*$') {
        throw "package binary imports a Visual C++ runtime DLL: $($binary.FullName)"
    }
}

# Smoke test without environment overrides: the compiler finds std\ and lib\
# from its own path. Then install, run, and uninstall through install.ps1.
$check = Join-Path $build 'check'
if (Test-Path -LiteralPath $check) { Remove-Item -LiteralPath $check -Recurse -Force }
New-Item -ItemType Directory -Path $check | Out-Null
Remove-Item Env:SYNX_STD_PATH, Env:SYNX_RUNTIME_LIB, Env:SYNX_RUNTIME_SHARED, Env:SYNX_LINKER, Env:SYNX_LINKER_DRIVER -ErrorAction SilentlyContinue
$launcher = Join-Path $stage 'bin\synx.cmd'
$workbench = Join-Path $check 'workbench.exe'
Invoke-WithoutToolchain {
    Invoke-Checked $launcher @('version')
    Invoke-Checked $launcher @('build', (Join-Path $stage 'examples\workbench\main.snx'), '-o', $workbench, '--linker=builtin')
    $null | & $workbench > (Join-Path $check 'workbench.out')
    if ($LASTEXITCODE -ne 0) { throw "workbench failed with exit code $LASTEXITCODE" }
}
$target = Join-Path $check 'installed Synx'
Invoke-Checked (Join-Path $stage 'install.cmd') @('-Destination', $target, '-NoPath')
Invoke-WithoutToolchain {
    $installedWorkbench = Join-Path $check 'workbench-installed.exe'
    # The package links with the built-in linker without a flag.
    $installedArgs = @('build', (Join-Path $stage 'examples\workbench\main.snx'), '-o', $installedWorkbench)
    Invoke-Checked (Join-Path $target 'bin\synx.cmd') $installedArgs
    $null | & $installedWorkbench > (Join-Path $check 'workbench-installed.out')
    if ($LASTEXITCODE -ne 0) { throw "installed workbench failed with exit code $LASTEXITCODE" }
}
Invoke-Checked (Join-Path $stage 'install.cmd') @('-Destination', $target, '-NoPath', '-Uninstall')
if (Test-Path -LiteralPath $target) { throw 'uninstall left files behind' }

New-Item -ItemType Directory -Path $out -Force | Out-Null
$zip = Join-Path $out "$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Push-Location -LiteralPath $stageRoot
try { Invoke-Checked cmake @('-E', 'tar', 'cf', $zip, '--format=zip', $name) } finally { Pop-Location }
Write-Output "Package: $zip"

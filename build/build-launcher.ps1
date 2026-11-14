param(
    [string]$PkgKeysDirectory,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$CMakePath,
    [string]$BuildDirectory,
    [string]$RuntimeStage
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'out\build' }
if (-not $PkgKeysDirectory) {
    $PkgKeysDirectory = Join-Path $root 'out\local-inputs'
    & (Join-Path $PSScriptRoot 'prepare-pkg-header.ps1') -OutputDirectory $PkgKeysDirectory
}
$keys = (Resolve-Path -LiteralPath $PkgKeysDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $keys 'pkg_keys.h') -PathType Leaf)) {
    throw 'PkgKeysDirectory must contain your locally supplied pkg_keys.h.'
}
if (-not $CMakePath) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { $CMakePath = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $CMakePath = (& $vswhere -latest -products '*' -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1)
        }
    }
}
if (-not $CMakePath -or -not (Test-Path -LiteralPath $CMakePath)) { throw 'Install CMake or supply -CMakePath.' }
& $CMakePath -S (Join-Path $root 'source') -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' "-DXSTATION_PKG_KEYS_DIR=$keys" "-DXSTATION_RUNTIME_STAGE=$RuntimeStage"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $CMakePath --build $BuildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'XStation build failed.' }
Write-Output "Built XStation: $BuildDirectory\$Configuration\XStation.exe"

param(
    [Parameter(Mandatory = $true)][string]$MsysRoot,
    [ValidateSet('CUSA00552','CUSA00900','CUSA02343','CUSA07408','CUSA08344')]
    [string[]]$Titles = @('CUSA00552','CUSA00900','CUSA02343','CUSA07408','CUSA08344'),
    [ValidateRange(1,32)][int]$Jobs = 3,
    [switch]$LTO
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path (Resolve-Path -LiteralPath $MsysRoot).Path 'clang64\bin'
$cmake = Join-Path $bin 'cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'MsysRoot must contain clang64/bin/cmake.exe. See docs/RUNTIMES.md.' }
$previousPath = $env:PATH
$env:PATH = $bin + ';' + $env:PATH
try {
    foreach ($title in $Titles) {
        $outDir = Join-Path $root "out\engines\$title"
        $ipo = if ($LTO) { 'ON' } else { 'OFF' }
        & $cmake -S (Join-Path $root "engines\$title\gpu") -B $outDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DBB_LTO=$ipo" '-DBB_PGO=off' '-DCMAKE_C_COMPILER=clang.exe' '-DCMAKE_CXX_COMPILER=clang++.exe'
        if ($LASTEXITCODE -ne 0) { throw "${title}: CMake configure failed." }
        & $cmake --build $outDir --target xstation-runtime --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw "${title}: Runtime build failed." }
        Write-Output "Built $title in $outDir"
    }
} finally { $env:PATH = $previousPath }

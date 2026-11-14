param(
    [Parameter(Mandatory = $true)][string]$MsysRoot,
    [string]$WslDistribution = 'Ubuntu',
    [string]$MesonPath = 'meson',
    [ValidateRange(1,32)][int]$Jobs = 3,
    [string]$BloodbornePatchFile
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
& (Join-Path $PSScriptRoot 'build-runtimes.ps1') -MsysRoot $MsysRoot -Jobs $Jobs
$linuxRoot = (& wsl.exe -d $WslDistribution -- wslpath -a $root).Trim()
if ($LASTEXITCODE -ne 0 -or -not $linuxRoot) { throw 'Cannot map repository into WSL.' }
foreach ($title in @('CUSA00552','CUSA00900','CUSA02343','CUSA07408','CUSA08344')) {
    & wsl.exe -d $WslDistribution -- env "MESON=$MesonPath" bash "$linuxRoot/build/build-mesa.sh" $title $Jobs
    if ($LASTEXITCODE -ne 0) { throw "${title}: Mesa source build failed." }
}
& (Join-Path $PSScriptRoot 'stage-runtimes.ps1') -MsysRoot $MsysRoot -BloodbornePatchFile $BloodbornePatchFile
& (Join-Path $PSScriptRoot 'build-launcher.ps1') -RuntimeStage (Join-Path $root 'out\runtime-stage')

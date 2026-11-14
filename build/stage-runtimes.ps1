param(
    [Parameter(Mandatory = $true)][string]$MsysRoot,
    [string[]]$Titles = @('CUSA00552','CUSA00900','CUSA02343','CUSA07408','CUSA08344'),
    [string]$BloodbornePatchFile
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $root 'out\runtime-stage'
$bin = Join-Path (Resolve-Path -LiteralPath $MsysRoot).Path 'clang64\bin'
$reader = Join-Path $bin 'llvm-readobj.exe'
if (-not (Test-Path -LiteralPath $reader)) { throw 'Install the CLANG64 LLVM tools.' }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$queue = [System.Collections.Generic.Queue[string]]::new()
$manifest = Get-Content -LiteralPath (Join-Path $root 'engines\manifest.json') -Raw | ConvertFrom-Json
foreach ($title in $Titles) {
    $entry = $manifest.engines | Where-Object title_id -eq $title
    if (-not $entry) { throw "Unknown engine: $title" }
    $runtime = Join-Path $root "out\engines\$title\$($entry.runtime_file)"
    $driver = Join-Path $root "out\mesa\$title\vulkan_dzn.dll"
    foreach ($file in @($runtime,$driver)) { if (-not (Test-Path -LiteralPath $file)) { throw "Build this component first: $file" } }
    $destination = Join-Path $stage "Engines\$title"
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $runtime -Destination $destination
    Copy-Item -LiteralPath $driver -Destination $destination
    $queue.Enqueue($runtime); $queue.Enqueue($driver)
    $defaults = Join-Path $root "engines\$title\defaults"
    if (Test-Path -LiteralPath $defaults) { Copy-Item -LiteralPath $defaults -Destination $destination -Recurse -Force }
    # Numeric reconstruction metadata contains offsets, lengths and hashes, never guest instructions.
    foreach ($recipe in Get-ChildItem -LiteralPath (Join-Path $root "engines\$title\preparation") -Filter '*.json' -ErrorAction SilentlyContinue) {
        $data = Get-Content -LiteralPath $recipe.FullName -Raw | ConvertFrom-Json
        $stream = [System.IO.MemoryStream]::new(); $writer = [System.IO.BinaryWriter]::new($stream)
        try {
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('XSRZ0002'))
            $writer.Write([uint32]$data.sites.Count)
            foreach ($value in [regex]::Matches($data.source_list_sha256,'..')) { $writer.Write([Convert]::ToByte($value.Value,16)) }
            foreach ($site in $data.sites) {
                $writer.Write([uint64]$site.offset); $writer.Write([uint16]$site.lengths.Count)
                foreach ($length in $site.lengths) { $writer.Write([sbyte]$length) }
                foreach ($value in [regex]::Matches($site.expected_sha256_prefix,'..')) { $writer.Write([Convert]::ToByte($value.Value,16)) }
            }
            $writer.Flush()
            $path = Join-Path $destination ($recipe.BaseName + '.bin')
            [System.IO.File]::WriteAllBytes($path,$stream.ToArray())
            if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $data.output_sha256) { throw "Reconstruction mismatch: $path" }
        } finally { $writer.Dispose(); $stream.Dispose() }
    }
    if ($title -eq 'CUSA00900' -and $BloodbornePatchFile) {
        if ((Get-FileHash -LiteralPath $BloodbornePatchFile -Algorithm SHA256).Hash -ne '3BCDE5D4485BA7B129550243740D3C377572669C6C7E8A6AA9D7D9CA2DB7CD6F') { throw 'Bloodborne patch input does not match the supported profile.' }
        Copy-Item -LiteralPath $BloodbornePatchFile -Destination (Join-Path $destination 'patches.bin')
        Copy-Item -LiteralPath $BloodbornePatchFile -Destination (Join-Path $destination 'patches-540p.bin')
    }
}
$seen = @{}
while ($queue.Count) {
    $file = $queue.Dequeue()
    if ($seen.ContainsKey($file)) { continue }; $seen[$file] = $true
    $imports = & $reader --coff-imports $file
    if ($LASTEXITCODE -ne 0) { throw "Import inspection failed: $file" }
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+Name: (.+\.dll)$') { continue }
        $name = $Matches[1]; $dependencyFile = Join-Path $bin $name
        if (Test-Path -LiteralPath $dependencyFile) {
            Copy-Item -LiteralPath $dependencyFile -Destination (Join-Path $stage $name) -Force
            $queue.Enqueue($dependencyFile)
        } elseif ($name -notmatch '^(api-ms-|ext-ms-|vcruntime|msvcp|vccorlib|ucrtbase)' -and -not (Test-Path -LiteralPath (Join-Path $env:WINDIR "System32\$name"))) {
            throw "Dependency unavailable from CLANG64 or Windows: $name (imported by $file)"
        }
    }
}
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'LICENSE'),(Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $licenses
$thirdPartyLicenses = Join-Path (Resolve-Path -LiteralPath $MsysRoot).Path 'clang64\share\licenses'
if (Test-Path -LiteralPath $thirdPartyLicenses) { Copy-Item -LiteralPath $thirdPartyLicenses -Destination (Join-Path $licenses 'msys2-dependencies') -Recurse -Force }
Write-Output "Runtime package content staged at $stage"
if ('CUSA00900' -in $Titles -and -not $BloodbornePatchFile) { Write-Warning 'Bloodborne on-console preparation still needs the matching locally supplied patch file. Runtime DLLs are built; game-specific patch payloads are not in the source repository.' }

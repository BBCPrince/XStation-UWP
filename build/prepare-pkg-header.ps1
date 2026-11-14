param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'out\local-inputs'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Numerics
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$sourcePath = Join-Path $OutputDirectory 'PkgKeySet.cs'
$revision = '5cd42a515dc29a453c7abcb93318493bdb5b5e1f'
$url = "https://raw.githubusercontent.com/pearlxcore/OrbisPkgTool/$revision/OrbisPkgTool/Crypto/PkgKeySet.cs"
if (-not (Test-Path -LiteralPath $sourcePath)) { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $sourcePath }
if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash -ne '62B8DEFBD2C8E33A793443EF1BCBEE61F5BB7805A632F32F797A4FE1AE908DD0') { throw 'Pinned OrbisPkgTool source hash mismatch.' }
$text = [System.IO.File]::ReadAllText($sourcePath)
function From-Hex([string]$Hex) {
    $hex = $Hex.Replace(' ', '')
    $little = [byte[]]::new(($hex.Length / 2) + 1)
    for ($i=0; $i -lt $hex.Length/2; $i++) { $little[$i] = [Convert]::ToByte($hex.Substring($hex.Length-2-2*$i, 2),16) }
    return [System.Numerics.BigInteger]::new($little)
}
function Inverse([System.Numerics.BigInteger]$Value, [System.Numerics.BigInteger]$Modulus) {
    $t=[System.Numerics.BigInteger]::Zero; $next=[System.Numerics.BigInteger]::One
    $r=$Modulus; $remaining=$Value % $Modulus
    while ($remaining -ne 0) {
        $quotient=[System.Numerics.BigInteger]::Divide($r,$remaining)
        $tmp=$t-$quotient*$next; $t=$next; $next=$tmp
        $tmp=$r-$quotient*$remaining; $r=$remaining; $remaining=$tmp
    }
    if ($r -ne 1) { throw 'No modular inverse.' }
    if ($t -lt 0) { $t += $Modulus }
    return $t
}
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('// Generated locally from the pinned MIT-licensed OrbisPkgTool PkgKeySet.cs.')
$lines.Add('// Copyright (c) 2026 pearlxcore. See THIRD_PARTY_NOTICES.md for the retained MIT notice.')
$lines.Add('#pragma once'); $lines.Add('#include <cstdint>')
$sizes = [ordered]@{Modulus=256; D=256; P=128; Q=128; DP=128; DQ=128; InverseQ=128; Exponent=3}
foreach ($block in @('DerivedKey3','FakeKeyset')) {
    $body=[regex]::Match($text, "$block\s*=\s*new RSAParameters\s*\{(.*?)\}", [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $body.Success) { throw "Missing RSA block: $block" }
    $values=@{}
    foreach ($name in $sizes.Keys) {
        $match=[regex]::Match($body.Groups[1].Value, "\b$name\s*=\s*Hex\(`"([0-9a-fA-F ]+)`"\)")
        if (-not $match.Success) { throw "Missing RSA field: $name" }
        $values[$name]=From-Hex $match.Groups[1].Value
    }
    if ($values.Modulus -ne $values.P*$values.Q -or ($values.Exponent*$values.D) % (($values.P-1)*($values.Q-1)) -ne 1) { throw 'RSA input consistency check failed.' }
    $values.DP=$values.D % ($values.P-1); $values.DQ=$values.D % ($values.Q-1)
    $values.InverseQ=Inverse $values.Q $values.P
    foreach ($name in $sizes.Keys) {
        $raw=$values[$name].ToByteArray(); $size=$sizes[$name]
        $bytes=[byte[]]::new($size)
        for ($i=0; $i -lt $raw.Length; $i++) {
            if ($i -lt $size) { $bytes[$size-1-$i]=$raw[$i] }
            elseif ($raw[$i] -ne 0) { throw 'RSA field exceeds its output size.' }
        }
        $literal=($bytes | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', '
        $lines.Add("static const uint8_t k${block}${name}[$size] = {$literal};")
    }
}
[System.IO.File]::WriteAllLines((Join-Path $OutputDirectory 'pkg_keys.h'), $lines, [System.Text.UTF8Encoding]::new($false))
Write-Output "Prepared local build header in $OutputDirectory (not tracked by Git)."

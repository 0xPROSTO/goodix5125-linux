# extract-psk.ps1 - run INSIDE Windows (PowerShell as Administrator) after a
# finger has been enrolled in Windows Hello with the Goodix 27c6:5125 sensor.
#
# Unseals the 32-byte TLS pre-shared key that the Goodix Windows driver keeps
# in C:\ProgramData\Goodix\Goodix_Cache.bin (DPAPI) and writes it to
# psk.bin on the Desktop. The key is unique to this sensor: never publish it.
#
# Entropy derivation as documented by the gxfp5120-linux / goodix-5117 projects.

param([string]$Out = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'psk.bin'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security

function HexBytes([string]$s) {
    $b = New-Object byte[] ($s.Length / 2)
    for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($s.Substring($i * 2, 2), 16) }
    return , $b
}

$cache = Join-Path $env:ProgramData 'Goodix\Goodix_Cache.bin'
if (-not (Test-Path $cache)) {
    Write-Host "Not found: $cache - is the Goodix driver installed and a finger enrolled?"
    exit 1
}

$data = [IO.File]::ReadAllBytes($cache)
$seed = $data[-8..-1]
$blob = $data[0..($data.Length - 9)]

$sha = [Security.Cryptography.SHA256]::Create()
$seedHash = $sha.ComputeHash([byte[]]$seed)
$c1 = HexBytes '9d7992b38402b66c81d1f555218942a9'
$c2 = HexBytes '1848d71550d270d219c80632ab4f8bb3'
$c3 = HexBytes 'e47c8938db5250f0205617ee17da4eb4'

$root = New-Object byte[] 16
for ($i = 0; $i -lt 8; $i++)  { $root[$i] = $c1[$i] -bxor $c2[$i] -bxor $c1[$i + 8] }
for ($i = 8; $i -lt 16; $i++) { $root[$i] = $c2[$i] -bxor $c3[$i] -bxor $c3[$i - 8] }
$entropy = [byte[]]($seedHash[16..31] + $sha.ComputeHash([byte[]]($seedHash[0..15] + $root)))

$out = $Out
foreach ($scope in 'LocalMachine', 'CurrentUser') {
    try {
        $psk = [Security.Cryptography.ProtectedData]::Unprotect(
            [byte[]]$blob, $entropy, [Security.Cryptography.DataProtectionScope]::$scope)
        if ($psk.Length -ne 32) { Write-Host "scope=${scope}: unexpected length $($psk.Length)"; continue }
        [IO.File]::WriteAllBytes($out, $psk)
        $h = ([BitConverter]::ToString($sha.ComputeHash($psk)) -replace '-', '').ToLower()
        Write-Host "OK (scope=$scope) -> $out"
        Write-Host "sha256(psk) = $h"
        exit 0
    } catch {
        Write-Host "scope=${scope} failed: $($_.Exception.Message)"
    }
}
Write-Host 'Could not unseal. Run as Administrator, in the account that enrolled the finger.'
exit 1

param(
    [Parameter(Mandatory = $true)]
    [string] $DllPath,

    [Parameter(Mandatory = $true)]
    [string] $VortiDeckRoot
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$dll = (Resolve-Path -LiteralPath $DllPath).Path
$vortideck = (Resolve-Path -LiteralPath $VortiDeckRoot).Path
$cargoManifest = Join-Path $vortideck 'apps\tauri-windows\src-tauri\Cargo.toml'
if (-not (Test-Path -LiteralPath $cargoManifest -PathType Leaf)) {
    throw "$vortideck is not a VortiDeck repository root."
}

$bytes = [IO.File]::ReadAllBytes($dll)
if ($bytes.Length -lt 0x86 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
    throw 'The selected file is not a Windows PE DLL.'
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length -or
    $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45) {
    throw 'The selected DLL has an invalid PE header.'
}
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne 0x8664) {
    throw ('Local staging currently accepts only Windows x64 DLLs; PE machine is 0x{0:X4}.' -f $machine)
}

$buildspec = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'buildspec.json') | ConvertFrom-Json
if ($buildspec.version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Invalid companion version in buildspec.json: $($buildspec.version)"
}
$compatibility = Get-Content -Raw -LiteralPath (Join-Path $repositoryRoot 'release-compatibility.json') | ConvertFrom-Json
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dll).Hash.ToUpperInvariant()
$destination = Join-Path $vortideck 'apps\tauri-windows\src-tauri\resources\plugins\vorti-obs-plugin.dll'
$manifestDestination = Join-Path $vortideck 'apps\tauri-windows\src-tauri\resources\obs-companion\manifest.json'

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifestDestination) | Out-Null
Copy-Item -LiteralPath $dll -Destination $destination -Force
[ordered]@{
    schemaVersion = 1
    id = [string] $compatibility.plugin.id
    version = [string] $buildspec.version
    protocolVersion = [int] $compatibility.plugin.protocolVersion
    platform = 'windows'
    architecture = 'x86_64'
    fileName = 'vorti-obs-plugin.dll'
    sha256 = $hash
} | ConvertTo-Json | Set-Content -LiteralPath $manifestDestination -Encoding utf8

Write-Host "Staged local companion $($buildspec.version) ($hash) into $vortideck"
Write-Host 'Rebuild VortiDeck to update the include_bytes! payload.'

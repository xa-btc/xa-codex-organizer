# Codex Organizer native resource contract v0.3.0
# Verifies the seven PNG-compressed ICO frames, version metadata, preview, and
# executable resource identity without launching or modifying the application.
# Last Updated: 2026-08-16 10:15:27

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$iconPath = Join-Path $projectRoot 'resources\codex-organizer.ico'
$previewPath = Join-Path $projectRoot 'resources\codex-organizer-icon.png'
$executable = Join-Path $projectRoot 'build\Release\CodexOrganizer.exe'
foreach ($path in @($iconPath, $previewPath, $executable)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing resource: $path" }
}

$bytes = [System.IO.File]::ReadAllBytes($iconPath)
if ([BitConverter]::ToUInt16($bytes, 0) -ne 0 -or
    [BitConverter]::ToUInt16($bytes, 2) -ne 1) {
    throw 'The CO file is not a Windows ICO container.'
}
$count = [BitConverter]::ToUInt16($bytes, 4)
if ($count -ne 7) { throw "Expected 7 ICO frames, found $count." }
$expected = @(256, 128, 64, 48, 32, 24, 16)
for ($index = 0; $index -lt $count; ++$index) {
    $entry = 6 + 16 * $index
    $width = [int]$bytes[$entry]
    if ($width -eq 0) { $width = 256 }
    $height = [int]$bytes[$entry + 1]
    if ($height -eq 0) { $height = 256 }
    $size = [BitConverter]::ToUInt32($bytes, $entry + 8)
    $offset = [BitConverter]::ToUInt32($bytes, $entry + 12)
    if ($width -ne $expected[$index] -or $height -ne $expected[$index]) {
        throw "ICO frame $index has unexpected dimensions ${width}x${height}."
    }
    if ($offset + $size -gt $bytes.Length -or
        $bytes[$offset] -ne 137 -or $bytes[$offset + 1] -ne 80 -or
        $bytes[$offset + 2] -ne 78 -or $bytes[$offset + 3] -ne 71) {
        throw "ICO frame $index is not a bounded PNG payload."
    }
}

$version = (Get-Item -LiteralPath $executable).VersionInfo
if ($version.FileVersion -ne '0.3.0' -or
    $version.ProductName -ne 'Codex Window Organizer') {
    throw "Executable version resource mismatch: $($version.FileVersion) / $($version.ProductName)"
}
if ((Get-Item -LiteralPath $previewPath).Length -lt 1024) {
    throw 'The CO icon preview is unexpectedly small.'
}

Write-Output 'RESOURCE_CONTRACT=PASS'
Write-Output 'ICO_SIZES=256,128,64,48,32,24,16'

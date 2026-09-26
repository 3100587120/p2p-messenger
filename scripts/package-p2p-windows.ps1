$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$release = Join-Path $repoRoot 'work/client-msvc/Release'
$destination = Join-Path $repoRoot 'dist/P2P-Messenger-Windows-Engine-Preview.zip'
if (Test-Path -LiteralPath $destination) {
    throw "Archive already exists: $destination"
}
if (-not (Test-Path -LiteralPath (Join-Path $release 'P2PMessenger.exe'))) {
    throw 'Build and deploy the Windows client first.'
}
$entries = @((Join-Path $release 'P2PMessenger.exe'))
$entries += @(Get-ChildItem -LiteralPath $release -Filter '*.dll' -File | ForEach-Object FullName)
foreach ($name in @('generic','iconengines','imageformats','networkinformation',
                     'platforms','qml','tls','translations')) {
    $path = Join-Path $release $name
    if (Test-Path -LiteralPath $path) { $entries += $path }
}
Compress-Archive -LiteralPath $entries -DestinationPath $destination -CompressionLevel Optimal
Write-Output $destination

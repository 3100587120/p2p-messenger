param([string]$Destination = '')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$release = Join-Path $repoRoot 'work/client-msvc/Release'
if (-not $Destination) {
    $Destination = Join-Path $repoRoot 'dist/ShuangDianLiao-Windows-Modes-Test.zip'
}
if (Test-Path -LiteralPath $Destination) {
    throw "Archive already exists: $Destination"
}
if (-not (Test-Path -LiteralPath (Join-Path $release 'P2PMessenger.exe'))) {
    throw 'Build and deploy the Windows client first.'
}
$friendlyExe = Join-Path $release '双点聊.exe'
Copy-Item -LiteralPath (Join-Path $release 'P2PMessenger.exe') -Destination $friendlyExe -Force
$entries = @($friendlyExe)
$entries += @(Get-ChildItem -LiteralPath $release -Filter '*.dll' -File | ForEach-Object FullName)
foreach ($name in @('generic','iconengines','imageformats','networkinformation',
                     'platforms','qml','tls','translations','multimedia')) {
    $path = Join-Path $release $name
    if (Test-Path -LiteralPath $path) { $entries += $path }
}
Compress-Archive -LiteralPath $entries -DestinationPath $Destination -CompressionLevel Optimal
Write-Output $Destination

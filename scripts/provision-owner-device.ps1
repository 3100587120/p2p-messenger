param([switch]$UploadSecret, [switch]$InstallOnThisPc, [switch]$EnableOwnerBinding)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$privateRoot = Join-Path $repoRoot '.private'
[void](New-Item -ItemType Directory -Path $privateRoot -Force)
$keyPath = Join-Path $privateRoot 'owner-device.key'
if (-not (Test-Path -LiteralPath $keyPath)) {
    # Generated credential, never printed or included in source/build packages.
    $bytes = [System.Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
    [System.IO.File]::WriteAllText($keyPath, [Convert]::ToBase64String($bytes))
}
$identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
& icacls $keyPath /inheritance:r /grant:r "${identity}:(F)" '*S-1-5-18:(F)' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Credential file ACL could not be restricted' }
if ($UploadSecret) {
    $env:XDG_CONFIG_HOME = Join-Path $privateRoot 'cli-config'
    $env:npm_config_prefix = Join-Path $repoRoot 'work/cloudflare-cli'
    $env:WRANGLER_SEND_METRICS = 'false'
    $env:CLOUDFLARE_ACCOUNT_ID = 'b4bc87290f34f6049116d65f761421d0'
    Push-Location (Join-Path $repoRoot 'relay')
    try {
        Get-Content -LiteralPath $keyPath -Raw | & node node_modules/wrangler/bin/wrangler.js secret put OWNER_DEVICE_SECRET
        if ($LASTEXITCODE -ne 0) { throw 'Owner privilege secret upload failed' }
    } finally { Pop-Location }
}
if ($InstallOnThisPc) {
    $env:QT_QPA_PLATFORM = 'offscreen'
    $arguments = @('--provision-owner-device', ('"' + $keyPath + '"'))
    if ($EnableOwnerBinding) { $arguments += '--enable-owner-binding' }
    $process = Start-Process -FilePath (Join-Path $repoRoot 'work/client-msvc/Release/P2PMessenger.exe') -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw 'Owner privilege encryption failed' }
}
Write-Output 'Owner credential operation completed; no credential values were logged.'

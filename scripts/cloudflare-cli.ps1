param([ValidateSet('Login','Whoami','Check','Deploy')][string]$Action = 'Whoami')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$env:XDG_CONFIG_HOME = Join-Path $repoRoot '.private/cli-config'
$env:npm_config_prefix = Join-Path $repoRoot 'work/cloudflare-cli'
$env:npm_config_cache = Join-Path $repoRoot 'work/npm-cache'
$env:TEMP = Join-Path $repoRoot 'work/build-temp'
$env:TMP = $env:TEMP
$env:WRANGLER_SEND_METRICS = 'false'
$env:CLOUDFLARE_ACCOUNT_ID = 'b4bc87290f34f6049116d65f761421d0'
Push-Location (Join-Path $repoRoot 'relay')
try {
    $cli = 'node_modules/wrangler/bin/wrangler.js'
    switch ($Action) {
        'Login' { & node $cli login --use-keyring --scopes account:read user:read workers_scripts:write }
        'Whoami' { & node $cli whoami }
        'Check' { & node $cli deploy --dry-run --outdir (Join-Path $repoRoot 'work/relay-dry-run') }
        'Deploy' { & node $cli deploy }
    }
    if ($LASTEXITCODE -ne 0) { throw "Cloudflare CLI failed: $LASTEXITCODE" }
} finally { Pop-Location }

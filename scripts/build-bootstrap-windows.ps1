$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repoRoot 'daemon/contrib/build/opendht'
$build = Join-Path $source 'build'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.19041.0'

$environmentLines = & cmd.exe /d /s /c '"D:\VSBuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul && set'
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$sdkInclude = Join-Path $sdkRoot "Include/$sdkVersion"
$env:INCLUDE = "$sdkInclude\ucrt;$sdkInclude\um;$sdkInclude\shared;$sdkInclude\winrt;$sdkInclude\cppwinrt;" + $env:INCLUDE
$env:LIB = 'D:\WinSDKLib;' + $env:LIB
$env:Path = (Join-Path $sdkRoot "bin/$sdkVersion/x64") + ';' + $env:Path
$env:UseEnv = 'true'
$env:WindowsSDKInstalled = 'true'
$env:WindowsSDK_Desktop_Support = 'true'

& cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 `
    -DOPENDHT_TOOLS=ON `
    "-DMSC_COMPAT_DIR=$($source.Replace('\', '/'))/src/compat/msvc" `
    "-DCMAKE_SYSTEM_VERSION=$sdkVersion" `
    '-DCMAKE_VS_GLOBALS=WindowsSDKInstalled=true;WindowsSDK_Desktop_Support=true;LibraryPath=D:/WinSDKLib' `
    "-DCMAKE_EXE_LINKER_FLAGS=/LIBPATH:D:/WinSDK10/Lib/$sdkVersion/um/x64 /LIBPATH:D:/WinSDK10/Lib/$sdkVersion/ucrt/x64"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $build --config Release --target dhtnode --parallel 8
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output (Join-Path $build 'tools/Release/dhtnode.exe')

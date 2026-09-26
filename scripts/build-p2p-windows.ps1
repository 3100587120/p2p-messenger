param([switch]$SkipDeploy, [switch]$BuildE2E)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$daemonRoot = Join-Path $repoRoot 'daemon'
$buildRoot = Join-Path $repoRoot 'work/client-msvc'
$qtRoot = Join-Path $repoRoot 'work/qt/6.7.3/msvc2019_64'
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

$configureArgs = @(
    '-S', (Join-Path $repoRoot 'client-p2p'),
    '-B', $buildRoot,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$($qtRoot.Replace('\', '/'))",
    "-DCMAKE_SYSTEM_VERSION=$sdkVersion",
    '-DCMAKE_VS_GLOBALS=WindowsSDKInstalled=true;WindowsSDK_Desktop_Support=true;LibraryPath=D:/WinSDKLib',
    "-DCMAKE_EXE_LINKER_FLAGS_INIT=/LIBPATH:D:/WinSDK10/Lib/$sdkVersion/um/x64 /LIBPATH:D:/WinSDK10/Lib/$sdkVersion/ucrt/x64",
    '-DP2P_MESSENGER_WITH_DAEMON=ON',
    "-DP2P_MESSENGER_DAEMON_INCLUDE_DIR=$($daemonRoot.Replace('\', '/'))/src",
    "-DP2P_MESSENGER_DAEMON_LIBRARY=$($daemonRoot.Replace('\', '/'))/build/x64/ReleaseLib_win32/bin/jami.lib"
)
if ($BuildE2E) { $configureArgs += '-DP2P_MESSENGER_BUILD_DAEMON_E2E=ON' }
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $buildRoot --config Release --parallel 8
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($BuildE2E) {
    & cmake --build $buildRoot --config Release --target P2PMessengerDaemonE2E --parallel 8
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $SkipDeploy) {
    $release = Join-Path $buildRoot 'Release'
    & (Join-Path $qtRoot 'bin/windeployqt.exe') --release --qmldir (Join-Path $repoRoot 'client-p2p/qml') (Join-Path $release 'P2PMessenger.exe') *> (Join-Path $repoRoot 'work/qt-deploy.log')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $ffmpegBin = Join-Path $daemonRoot 'contrib/build/ffmpeg/Build/win32/x64/bin'
    foreach ($name in @('avcodec-58.dll','avdevice-58.dll','avfilter-7.dll','avformat-58.dll','avutil-56.dll','swresample-3.dll','swscale-5.dll')) {
        Copy-Item -LiteralPath (Join-Path $ffmpegBin $name) -Destination $release -Force
    }
    foreach ($name in @('libcrypto-1_1-x64.dll','libssl-1_1-x64.dll')) {
        Copy-Item -LiteralPath (Join-Path $daemonRoot "contrib/build/openssl/$name") -Destination $release -Force
    }
    $crtRoot = 'D:\VSBuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT'
    foreach ($name in @('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll',
                         'msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll',
                         'vcruntime140.dll','vcruntime140_1.dll','vcruntime140_threads.dll')) {
        Copy-Item -LiteralPath (Join-Path $crtRoot $name) -Destination $release -Force
    }
}

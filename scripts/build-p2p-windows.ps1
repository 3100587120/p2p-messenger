param([switch]$SkipDeploy, [switch]$BuildE2E, [switch]$BuildRelayTests,
      [string]$RelayUrl = '')

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$daemonRoot = Join-Path $repoRoot 'daemon'
$buildRoot = Join-Path $repoRoot 'work/client-msvc'
$qtRoot = Join-Path $repoRoot 'work/qt/6.7.3/msvc2019_64'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.19041.0'
$cmake = 'D:\VSBuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

$environmentLines = & cmd.exe /d /s /c '"D:\VSBuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul && set'
$developerPath = ''
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        if ($matches[1] -ieq 'PATH') {
            if ($matches[1] -ceq 'PATH' -or -not $developerPath) { $developerPath = $matches[2] }
            continue
        }
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if (-not $developerPath) { throw 'Visual Studio developer PATH was not returned' }
# The launcher can provide both PATH and Path; MSBuild rejects the duplicate.
foreach ($key in @([Environment]::GetEnvironmentVariables('Process').Keys)) {
    if ($key -ieq 'PATH') { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
}
[Environment]::SetEnvironmentVariable('Path', $developerPath, 'Process')
$sdkInclude = Join-Path $sdkRoot "Include/$sdkVersion"
$env:INCLUDE = "$sdkInclude\ucrt;$sdkInclude\um;$sdkInclude\shared;$sdkInclude\winrt;$sdkInclude\cppwinrt;" + $env:INCLUDE
$env:LIB = 'D:\WinSDKLib;' + $env:LIB
$env:Path = (Join-Path $sdkRoot "bin/$sdkVersion/x64") + ';' + $env:Path
$env:UseEnv = 'true'
$env:WindowsSDKInstalled = 'true'
$env:WindowsSDK_Desktop_Support = 'true'

function Invoke-CMake([string[]]$Arguments) {
    $start = [System.Diagnostics.ProcessStartInfo]::new($cmake)
    $start.UseShellExecute = $false
    $start.Environment.Clear()
    foreach ($key in [Environment]::GetEnvironmentVariables('Process').Keys) {
        $start.Environment[[string]$key] = [Environment]::GetEnvironmentVariable([string]$key, 'Process')
    }
    foreach ($argument in $Arguments) { [void]$start.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($start)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "CMake failed with exit code $($process.ExitCode)" }
}

$configureArgs = @(
    '-S', (Join-Path $repoRoot 'client-p2p'),
    '-B', $buildRoot,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$($qtRoot.Replace('\', '/'))",
    "-DCMAKE_SYSTEM_VERSION=$sdkVersion",
    '-DCMAKE_VS_GLOBALS=WindowsSDKInstalled=true;WindowsSDK_Desktop_Support=true;LibraryPath=D:/WinSDKLib',
    "-DCMAKE_EXE_LINKER_FLAGS_INIT=/LIBPATH:D:/WinSDK10/Lib/$sdkVersion/um/x64 /LIBPATH:D:/WinSDK10/Lib/$sdkVersion/ucrt/x64",
    '-DP2P_MESSENGER_WITH_DAEMON=ON',
    "-DP2P_MESSENGER_DEFAULT_RELAY_URL=$RelayUrl",
    '-DCMAKE_AUTOGEN_PARALLEL=1',
    "-DP2P_MESSENGER_BUILD_DAEMON_E2E=$($BuildE2E.IsPresent.ToString().ToUpperInvariant())",
    "-DP2P_MESSENGER_BUILD_RELAY_TESTS=$($BuildRelayTests.IsPresent.ToString().ToUpperInvariant())",
    "-DP2P_MESSENGER_DAEMON_INCLUDE_DIR=$($daemonRoot.Replace('\', '/'))/src",
    "-DP2P_MESSENGER_DAEMON_LIBRARY=$($daemonRoot.Replace('\', '/'))/build/x64/ReleaseLib_win32/bin/jami.lib"
)
if ($BuildE2E) { $configureArgs += '-DP2P_MESSENGER_BUILD_DAEMON_E2E=ON' }
Invoke-CMake $configureArgs
Invoke-CMake @('--build', $buildRoot, '--config', 'Release', '--target', 'P2PMessenger', '--parallel', '1', '--', '/nr:false')
if ($BuildE2E) {
    Invoke-CMake @('--build', $buildRoot, '--config', 'Release', '--target', 'P2PMessengerDaemonE2E', '--parallel', '1', '--', '/nr:false')
}
if ($BuildRelayTests) {
    Invoke-CMake @('--build', $buildRoot, '--config', 'Release', '--target', 'P2PMessengerFriendFlowTest', '--parallel', '1', '--', '/nr:false')
    Invoke-CMake @('--build', $buildRoot, '--config', 'Release', '--target', 'P2PMessengerRelayCryptoTest', '--parallel', '1', '--', '/nr:false')
    Invoke-CMake @('--build', $buildRoot, '--config', 'Release', '--target', 'P2PMessengerRelayClientTest', '--parallel', '1', '--', '/nr:false')
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

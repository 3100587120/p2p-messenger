param(
    [string]$BuildRoot = "",
    [string]$RelayUrl = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo 'work' }
$env:TEMP = Join-Path $BuildRoot 'build-temp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
$sdk = Join-Path $BuildRoot 'android-sdk'
$ndk = Join-Path $sdk 'ndk/29.0.14206865'
$cmake = Join-Path $sdk 'cmake/4.1.2/bin/cmake.exe'
$qt = Join-Path $BuildRoot 'qt/6.7.3/android_arm64_v8a'
$qtHost = Join-Path $BuildRoot 'qt/6.7.3/mingw_64'
$jdk = Join-Path $BuildRoot 'jdk17/jdk-17.0.20.1+1'
$daemon = Join-Path $repo 'client-android/daemon'
$contrib = Join-Path $daemon 'contrib/aarch64-linux-android'
$daemonBuild = Join-Path $BuildRoot 'daemon-android-arm64-v5'
$enginePackage = Join-Path $BuildRoot 'android-engine-package/libjami-core.so'
$appBuild = Join-Path $BuildRoot 'client-p2p-android-debug'
$dist = Join-Path $repo 'dist/ShuangDianLiao-Android-arm64-AssistedProxy-Test-debug.apk'
$opensslRoot = Join-Path $BuildRoot 'android_openssl'
$opensslCommit = 'b71f1470962019bd89534a2919f5925f93bc5779'
$opensslLib = Join-Path $opensslRoot 'ssl_3/arm64-v8a'

if (-not (Test-Path -LiteralPath (Join-Path $opensslLib 'libssl_3.so'))) {
    New-Item -ItemType Directory -Force -Path $opensslRoot | Out-Null
    & git -C $opensslRoot init -q
    if ($LASTEXITCODE) { throw 'Unable to initialize Android OpenSSL dependency' }
    & git -C $opensslRoot remote add origin 'https://github.com/KDAB/android_openssl.git'
    if ($LASTEXITCODE) { throw 'Unable to configure Android OpenSSL source' }
    & git -C $opensslRoot fetch --depth 1 origin $opensslCommit
    if ($LASTEXITCODE) { throw 'Unable to fetch Android OpenSSL dependency' }
    & git -C $opensslRoot checkout -q FETCH_HEAD
    if ($LASTEXITCODE) { throw 'Unable to check out Android OpenSSL dependency' }
}
foreach ($name in @('libcrypto_3.so', 'libssl_3.so')) {
    if (-not (Test-Path -LiteralPath (Join-Path $opensslLib $name))) {
        throw "Missing Android TLS library: $name"
    }
}

foreach ($required in @($cmake, (Join-Path $jdk 'bin/java.exe'),
                       (Join-Path $contrib 'lib/libyrs.a'),
                       (Join-Path $contrib 'lib/libupnp.a'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing Android build prerequisite: $required" }
}

$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_NDK_ROOT = $ndk
$env:JAVA_HOME = $jdk
$env:GRADLE_USER_HOME = Join-Path $BuildRoot 'gradle-user-home'
$env:PATH = "$(Join-Path $jdk 'bin');$(Join-Path $sdk 'cmake/4.1.2/bin');D:\Tools\msys64\usr\bin;$env:PATH"
$msysContrib = '/' + $repo.Substring(0, 1).ToLowerInvariant() + '/' +
    $repo.Substring(3).Replace('\', '/') + '/client-android/daemon/contrib/aarch64-linux-android/lib/pkgconfig'
$env:PKG_CONFIG_PATH = ''
$env:PKG_CONFIG_LIBDIR = $msysContrib

& $cmake -S $daemon -B $daemonBuild -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" `
    '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-26' '-DCMAKE_BUILD_TYPE=Release' `
    '-DBUILD_CONTRIB=OFF' '-DJAMI_JNI=OFF' '-DJAMI_DBUS=OFF' '-DJAMI_VIDEO=OFF' `
    '-DJAMI_PLUGIN=OFF' '-DBUILD_SHARED_LIBS=ON' '-DBUILD_TESTING=OFF' `
    '-DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=OFF'
if ($LASTEXITCODE) { throw 'Android engine configure failed' }
& $cmake --build $daemonBuild --parallel 8
if ($LASTEXITCODE) { throw 'Android engine build failed' }

New-Item -ItemType Directory -Force -Path (Split-Path $enginePackage) | Out-Null
Copy-Item -LiteralPath (Join-Path $daemonBuild 'libjami-core.so') -Destination $enginePackage -Force
& (Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe') --strip-unneeded $enginePackage
if ($LASTEXITCODE) { throw 'Android engine strip failed' }

& $cmake -S (Join-Path $repo 'client-p2p') -B $appBuild -G Ninja `
    "-DCMAKE_TOOLCHAIN_FILE=$qt/lib/cmake/Qt6/qt.toolchain.cmake" `
    "-DQT_HOST_PATH=$qtHost" "-DANDROID_SDK_ROOT=$sdk" `
    '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-26' '-DCMAKE_BUILD_TYPE=Debug' `
    '-DP2P_MESSENGER_WITH_DAEMON=ON' `
    "-DP2P_MESSENGER_ANDROID_OPENSSL_DIR=$opensslLib" `
    "-DP2P_MESSENGER_DEFAULT_RELAY_URL=$RelayUrl" `
    '-DCMAKE_AUTOGEN_PARALLEL=1' `
    "-DP2P_MESSENGER_DAEMON_INCLUDE_DIR=$daemon/src" `
    "-DP2P_MESSENGER_DAEMON_LIBRARY=$enginePackage"
if ($LASTEXITCODE) { throw 'Android app configure failed' }
& $cmake --build $appBuild --parallel 8
if ($LASTEXITCODE) { throw 'Android APK build failed' }

$apk = Join-Path $appBuild 'android-build/P2PMessenger.apk'
if (-not (Test-Path -LiteralPath $apk)) { throw "APK not found: $apk" }
New-Item -ItemType Directory -Force -Path (Split-Path $dist) | Out-Null
Copy-Item -LiteralPath $apk -Destination $dist -Force
Write-Output "APK: $dist"
Write-Output "SHA-256: $((Get-FileHash -LiteralPath $dist -Algorithm SHA256).Hash)"

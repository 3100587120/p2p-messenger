# P2P Messenger client

Original Qt Quick interface over an embedded Jami engine. It creates a local
identity, handles trust-based friend requests, private and group conversations,
and peer-to-peer file offers. The interface does not load remote assets or
contact an upstream service.

## Windows build and verification

The current verified build uses Qt 6.7.3 MSVC, the local MSVC Jami library,
and the tools under `D:\p2p-messenger\work`. From PowerShell:

```powershell
./scripts/build-p2p-windows.ps1 -BuildE2E
```

The executable is `work/client-msvc/Release/P2PMessenger.exe`. The build
script deploys Qt and runtime DLLs into that same directory. To run the
integration test, start a self-hosted OpenDHT node on UDP 4222 with no public
bootstrap (`dhtnode -d -p 4222`), then run:

```powershell
./work/client-msvc/Release/P2PMessengerDaemonE2E.exe ./work/e2e-profiles 127.0.0.1:4222
```

The test creates two isolated identities and verifies a live friend request,
acceptance, private message, P2P file transfer, group invitation, and group
message. It never uses a public Jami bootstrap.

## Android local build

The arm64 Android client now links the private Jami daemon and includes an
Android document-URI bridge for file sending and saving. After installing the
Qt 6.7.3 Android kit, Android SDK/NDK and JDK 17 under `work` on D:, build
the native contrib once, then use:

```powershell
./scripts/build-p2p-android.ps1
```

The incremental script rebuilds only changed native objects and creates an
installable, debug-signed APK at
`dist/P2P-Messenger-Android-arm64-Private-Engine-debug.apk`. It keeps Gradle's
cache on D:. A debug signature is for testing, not store distribution.

The Android build has passed compilation, APK packaging, signature validation,
and inspection confirming that `libjami-core.so` is in the APK. No Android
device was connected for launch or two-device messaging tests. Those runtime
checks and a first-party TURN relay test remain required before release.

## Current limits

This is not a completed cross-platform release. Older Android APKs in `dist`
are UI-only shells; use the `Private-Engine` APK above for current testing.
The Android daemon API adapter compiles, but device-level behavior is still
unverified. iOS has not been validated.

The Qt-side chat cache is AES-GCM encrypted with a device-protected key. The
embedded Jami engine also keeps its own local Git conversation repository,
which is not yet encrypted at rest. Do not describe the full local history as
encrypted until that store is protected and audited. A self-hosted DHT node
provides discovery; cross-NAT file relay requires a separately operated TURN
server and has not been verified end-to-end.

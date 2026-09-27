# 双点聊 client

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

The executable is `work/client-msvc/Release/P2PMessenger.exe`; the portable
package is `dist/ShuangDianLiao-Windows-Direct.zip` and starts with
`双点聊.exe`. The executable embeds the new icon. The build script deploys Qt
and runtime DLLs into the release directory unless `-SkipDeploy` is used. To run the
integration test, start a self-hosted OpenDHT node on UDP 4222 with no public
bootstrap (`dhtnode -d -p 4222`), then run:

```powershell
./work/client-msvc/Release/P2PMessengerDaemonE2E.exe ./work/e2e-profiles 127.0.0.1:4222
```

The test creates two isolated identities and verifies a live friend request,
acceptance, private message, P2P file transfer, group invitation, and group
message. It never uses a public Jami bootstrap.

For a strict two-peer test with no separate DHT node, use:

```powershell
./work/client-msvc/Release/P2PMessengerDaemonE2E.exe ./work/e2e-profiles --direct
```

The first identity opens a DHT port and the second bootstraps directly from it.
With LAN peer discovery enabled, this test passed friend requests, private
text, P2P files, group invitations, and group text without a separate node.
It exercises the topology on one computer, not two physical devices or NATs.

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
`dist/ShuangDianLiao-Android-arm64-Direct-debug.apk` after copying the Qt
package from `work/client-p2p-android-debug/android-build/P2PMessenger.apk`.
It keeps Gradle's
cache on D:. A debug signature is for testing, not store distribution.

The Android build has passed compilation, APK packaging, signature validation,
and inspection confirming that `libjami-core.so` is in the APK. No Android
device was connected for launch or two-device messaging tests. Those runtime
checks remain required before release; strict direct mode never uses TURN.

## Strict two-device pairing

The single Connect Device dialog lists nearby LAN accounts for one-tap adding
and accepts a pairing code from another network. Share My Pairing Code
automatically includes up to two globally addressed IPv6 endpoints and a
private IPv4 endpoint when available. The other device can paste it without
entering a name or address manually. Action failures appear in a modal dialog
with a suggested next step; an unconfirmed connection prompts after one minute.
The app uses the peer as its DHT entry point, not a hosted service. There is
no TURN relay. On the same LAN, the Jami ID alone can use local peer discovery.
The suggested endpoints use the actual bound DHT port. Across networks, an
address must still be publicly reachable (for example, global IPv6 or a
manually forwarded UDP port). The app does not automatically map router ports,
discover public IPv4 addresses, or verify reachability, so connection success
is not guaranteed behind restrictive NATs. Advanced settings remain available
for a manually reachable address.

## Current limits

This is not a completed cross-platform release. Older Android APKs in `dist`
are UI-only shells; use the `ShuangDianLiao` APK above for current testing.
The Android daemon API adapter compiles, but device-level behavior is still
unverified. iOS has not been validated. The displayed product name is 双点聊;
the internal package ID and data-directory identity remain unchanged so an
update does not orphan existing local accounts and history.

The Qt-side chat cache is AES-GCM encrypted with a device-protected key. The
embedded Jami engine also keeps its own local Git conversation repository,
which is not yet encrypted at rest. Do not describe the full local history as
encrypted until that store is protected and audited. If both devices are
behind restrictive NATs, connection failure is expected rather than silently
using a third-party relay.

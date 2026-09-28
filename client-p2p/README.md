# 双点聊 client

Qt Quick interface over an embedded Jami engine, with a separate opt-in
encrypted Cloudflare relay transport. The interface does not load remote
assets. Strict Direct is the default and does not contact a public service.

## Windows build and verification

The current verified build uses Qt 6.7.3 MSVC, the local MSVC Jami library,
and the tools under `D:\p2p-messenger\work`. From PowerShell:

```powershell
./scripts/build-p2p-windows.ps1 -BuildE2E
```

The executable is `work/client-msvc/Release/P2PMessenger.exe`; the portable
current test package is `dist/ShuangDianLiao-Windows-TrustFix-Test.zip` and starts
with `双点聊.exe`. The executable embeds the new icon. The build script deploys Qt
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
`dist/ShuangDianLiao-Android-arm64-TrustFix-Test-debug.apk` after copying the Qt
package from `work/client-p2p-android-debug/android-build/P2PMessenger.apk`.
It keeps Gradle's
cache on D:. A debug signature is for testing, not store distribution. The
two-mode test APK passed APK signature verification and contains the embedded
Jami engine, but has not been installed on a physical Android device.

The Android build has passed compilation, APK packaging, signature validation,
and inspection confirming that `libjami-core.so` is in the APK. No Android
device was connected for launch or two-device messaging tests. Those runtime
checks remain required before release; strict direct mode never uses TURN.

## Connection modes and two-device pairing

The default is Strict Direct. It does not use public bootstrap, proxy, STUN,
name, or TURN services. Assisted Connection uses the separately deployed
Cloudflare Worker under the user's account, not Jami's public DHT/TURN. Both
devices must opt in and exchange new `SD1-` public-key pairing codes. The
Worker forwards encrypted packets only while the recipient is online; it
stores no message history. Unacknowledged packets are encrypted and queued on
the sender's device, then retried when it reconnects. "Delivered" is shown
only after an authenticated acknowledgement from the recipient device.
The relay can see connection metadata, but not message or file plaintext.
It is a third-party service in Assisted mode; switching back disables its
WebSocket connection. Jami is configured without public DHT/TURN endpoints
in both modes.

For release builds, the operator passes the deployed `wss://...workers.dev`
endpoint as `-RelayUrl` to both `scripts/build-p2p-windows.ps1` and
`scripts/build-p2p-android.ps1`. It becomes the Assisted Connection default
inside both apps; users only select the mode and exchange SD1 pairing codes.
The default Strict Direct mode does not connect to it. A build without
`-RelayUrl` has no embedded service address and cannot enable Assisted mode
until a user explicitly configures an endpoint under **本机账号 → 高级网络设置**.
That override is saved locally. For scripted desktop tests,
`P2P_MESSENGER_RELAY_URL` overrides the saved/build address at startup.
The Worker source and tests are in `relay/`.
For a temporary WAN trial without account deployment, `wrangler dev --local
--tunnel` in `relay/` gives a random `*.trycloudflare.com` address. Both clients
must enter its `wss://` form and the local Wrangler process must stay running.
Cloudflare documents Quick Tunnels as development-only: the address changes on
restart and availability is not guaranteed. Do not treat such a trial as a
permanent relay deployment.
It supports friend requests, private messages, group invitations/messages,
and private file transfers up to 2 MB. Files and chat history remain in the
local vault; the Worker does not provide offline cloud storage. This transport
has passed local protocol tests, but not a physical two-network acceptance
test. Do not present it as WAN-validated yet.

The upstream [Jami LAN guide](https://docs.jami.net/en_US/user/lan-only.html)
explains the direct-mode topology. Existing packages in `dist/` predate this
transport; do not treat them as validated relay releases.

The Add Friend dialog lists nearby LAN accounts for one-tap adding
and accepts a pairing code from another network. Share My Pairing Code
automatically includes up to two globally addressed IPv6 endpoints and a
private IPv4 endpoint when available. The other device can paste it without
entering a name or address manually. Action failures appear in a modal dialog
with a suggested next step; an unconfirmed connection prompts after one minute.
Incoming friend requests now open an in-app dialog, and a persistent View Friend
Requests button allows manually refreshing the pending list. This requires the
recipient app to be running; no background push service is configured. A sent
request is queued locally first, not proof that the recipient has received it.
In Strict Direct mode, the app uses the peer as its DHT entry point, not a
hosted service. There is no TURN relay. On the same LAN, the Jami ID alone can
use local peer discovery.
The suggested endpoints use the actual bound DHT port. Across networks, the
client first tries local-router UPnP IGD (SSDP discovery, external-address
query, a one-hour UDP port mapping with renewal). It only talks to a private
numeric gateway address and does not call a third-party discovery service.
If the gateway returns a public IPv4 address and accepts the mapping, that
endpoint is included in the pairing code alongside global IPv6 and LAN IPv4.
The client rejects carrier-grade/private WAN addresses. UPnP is not supported
by every router and no serverless technique guarantees a connection through
restrictive NATs. It does not implement PCP/NAT-PMP or independently verify
reachability from outside the LAN yet. Advanced settings remain available for
a manually reachable address.

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
behind restrictive NATs, Strict Direct may fail, but never silently switches
to a third-party relay. The user can explicitly select Assisted Connection.

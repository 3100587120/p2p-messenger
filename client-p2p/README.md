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

## Current limits

This is not a completed cross-platform release. The existing Android APKs in
`dist` are UI-only shells and do not contain the Jami engine. The Android
daemon checked out in `client-android/daemon` has a newer, incompatible API;
its custom build and adapter are still required. iOS has not been validated.

The Qt-side chat cache is AES-GCM encrypted with a device-protected key. The
embedded Jami engine also keeps its own local Git conversation repository,
which is not yet encrypted at rest. Do not describe the full local history as
encrypted until that store is protected and audited. A self-hosted DHT node
provides discovery; cross-NAT file relay requires a separately operated TURN
server and has not been verified end-to-end.

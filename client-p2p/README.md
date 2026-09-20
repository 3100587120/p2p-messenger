# P2P Messenger client

This is the original Qt Quick application shell for Windows, Android, and iOS.
It contains no upstream Jami visual assets, web content, analytics, or remote
configuration. The initial UI provides the shared interaction model for local
identity, verified friend invitations, conversations, and file-transfer state.

The next integration layer binds `MessengerController` to the private daemon
configuration: it creates a local identity, validates an invite, persists the
encrypted local history, and uses only explicitly configured first-party
rendezvous/TURN endpoints when direct delivery is unavailable.

## Build prerequisites

- Qt 6.5 or newer with Qt Quick
- CMake 3.21 or newer
- A platform toolchain (MSVC for Windows; Android SDK/NDK; Xcode on macOS for
  iOS)

```
cmake -S client-p2p -B build/client-p2p
cmake --build build/client-p2p --config Release
```

# P2P Messenger client

This is the original Qt Quick application shell for Windows, Android, and iOS.
It contains no upstream Jami visual assets, web content, analytics, or remote
configuration. The initial UI provides the shared interaction model for local
identity, verified friend invitations, conversations, and file-transfer state.

`DaemonBridge` is the only integration boundary for the private daemon. It
creates a local identity, adds a verified contact, creates private or group
conversations, sends messages, and starts file transfers. It is compiled only
when given the private daemon headers and library, so an accidental upstream
library or network configuration cannot silently enter a release build.

## Build prerequisites

- Qt 6.5 or newer with Qt Quick
- CMake 3.21 or newer
- A platform toolchain (MSVC for Windows; Android SDK/NDK; Xcode on macOS for
  iOS)

```
cmake -S client-p2p -B build/client-p2p
cmake --build build/client-p2p --config Release
```

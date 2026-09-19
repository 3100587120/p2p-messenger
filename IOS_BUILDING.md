# iOS build guide and verified lessons

## Verified result

The iOS Simulator build is verified on GitHub Actions:

- Successful run: <https://github.com/3100587120/p2p-messenger/actions/runs/35459018522>
- Verified commit: `d4dc7f5`
- Runner: `macos-15-intel`
- Xcode: 16.4
- Target: iOS Simulator, `x86_64`, Debug, signing disabled
- First clean build time: about 59 minutes

iOS cannot be built natively on this Windows workstation because the Apple SDK,
simulator runtime, and `xcodebuild` are only available through Xcode on macOS.
Windows remains the control workstation; GitHub Actions provides the macOS build
machine.

## Final source layout

The working build uses the current Jami iOS client instead of the old client
pinned by the historical `jami-project` superproject.

- Main repository: `3100587120/p2p-messenger`
- iOS client fork: `3100587120/p2p-messenger-ios`, branch `p2p-current`
- Daemon fork: `3100587120/p2p-messenger-daemon`
- The iOS client pins its matching daemon revision as a nested submodule.
- All nested sources are fetched from GitHub rather than Jami Gerrit.

Keep the iOS client and its daemon revision together. Updating only one side can
create API, framework, or linker incompatibilities.

## Build pipeline

The workflow at `.github/workflows/ios-simulator.yml`:

1. Checks out all pinned submodules recursively.
2. Installs Carthage, Node.js, Autotools, libtool, pkg-config, and Yasm.
3. Keys the native cache by daemon commit, Xcode version, architecture, and
   `compile-ios.sh` content.
4. Restores or builds the Jami core for the `x86_64` simulator.
5. Saves `DEPS` and generated XCFrameworks immediately after core success.
6. Restores or fetches Carthage dependencies and saves them immediately.
7. Builds `Ring.xcodeproj` without code signing.

Saving caches before the final Xcode step means a Swift compilation failure no
longer forces the next run to rebuild the C++ core and Carthage dependencies.

Expected timing:

- First clean build: approximately 50–65 minutes.
- Build with valid core and Carthage caches: approximately 8–15 minutes.
- Cache misses are expected after changing the daemon revision, Xcode version,
  simulator architecture, native build script, or `Cartfile.resolved`.

## Compatibility decisions

### Match the simulator architecture

The native core is built with:

```sh
./compile-ios.sh --platform=iPhoneSimulator --arch=x86_64
```

The Xcode build must use the same architecture:

```text
ARCHS=x86_64
ONLY_ACTIVE_ARCH=YES
```

Otherwise Xcode attempts to link an `arm64` simulator app against x86-only
XCFrameworks and fails with undefined symbols for arm64.

### Compile newer SDK APIs conditionally

The current upstream client contains APIs introduced with the iOS 26 SDK, while
the runner provides Xcode 16.4 and the iOS 18.5 SDK. A runtime-only check such as
`#available(iOS 26.0, *)` is insufficient when the older compiler does not know
the symbol.

New SDK-only symbols are enclosed in:

```swift
#if compiler(>=6.2)
// iOS 26 SDK API, plus its runtime availability check
#else
// compatible fallback
#endif
```

The audited symbols include `scrollEdgeEffectHidden`, `glassEffect`,
`listSectionMargins`, and the iOS 26.4 PushKit metadata callback. The fallback
UI remains available on iOS 15–18.

## Problems encountered and durable fixes

| Symptom | Root cause | Durable fix |
| --- | --- | --- |
| Python syntax failure | Old script assumed Python 2 | Move to the current iOS client/toolchain |
| FFmpeg/LibreSSL checksum failures | Historical archives changed | Stop maintaining the obsolete 2021 dependency set |
| Missing Yasm or `aclocal` | Native requirements were incomplete | Install the documented tools and provide the expected `aclocal` command |
| Recursive checkout stalled | Nested daemon came from Jami Gerrit | Point the matched daemon to the GitHub fork |
| SwiftUI symbols not found | iOS 26 APIs were compiled with Xcode 16.4 | Add compiler-level guards and fallbacks |
| Undefined symbols for arm64 | Core was x86_64 while Xcode requested arm64 too | Set `ARCHS=x86_64` and `ONLY_ACTIVE_ARCH=YES` |
| Job rejected before starting | Private-repository macOS billing was unavailable | Publish this GPL repository for public Actions runners |
| Every retry took nearly an hour | Completed outputs were discarded | Save core and Carthage caches before Xcode |

## Recommended diagnostic order

1. Confirm the job started. A job with no steps usually means billing, runner,
   or workflow validation trouble rather than a source error.
2. Confirm recursive checkout completed and no submodule uses an inaccessible
   remote.
3. Check whether the core and Carthage cache steps were hits or misses.
4. If the core fails, diagnose native dependencies before touching Swift code.
5. If Xcode fails, extract `error:`, fatal-error, undefined-symbol, and failed
   command lines before reading the surrounding log.
6. For a missing Apple API, search every use and related availability check.
7. For linker errors, compare the native and Xcode target architectures first.
8. Trigger only one replacement run and cancel accidental duplicates.

Useful commands on this workstation:

```powershell
& 'D:\New Folder\gh.exe' run list --repo 3100587120/p2p-messenger --workflow ios-simulator.yml
& 'D:\New Folder\gh.exe' run view RUN_ID --repo 3100587120/p2p-messenger --log-failed
& 'D:\New Folder\gh.exe' workflow run ios-simulator.yml --repo 3100587120/p2p-messenger --ref master
```

## Remaining boundary

This workflow proves that the app compiles for the iOS Simulator. An installable
device IPA or TestFlight release still requires an Apple Developer team, signing
certificate, provisioning profiles, bundle identifiers, and App Store Connect
credentials. Those secrets are intentionally not stored in this repository.

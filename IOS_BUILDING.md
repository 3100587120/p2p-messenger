# iOS build from this Windows workstation

iOS binaries cannot be built natively on Windows: Apple provides the iPhoneOS
SDK, `xcodebuild`, simulator runtimes, and distribution signing only in Xcode
on macOS. This repository therefore uses a macOS GitHub Actions runner for the
actual compilation.

The workflow at `.github/workflows/ios-simulator.yml` does the following on a
macOS runner:

1. Checks out this repository and its pinned submodules.
2. Compiles the Jami daemon and dependencies for the iOS Simulator.
3. Builds the Ring iOS client with code signing disabled.

It is deliberately a simulator build. Producing an installable device IPA or
uploading to TestFlight requires an Apple Developer team, signing certificate,
provisioning profile, and App Store Connect credentials. Those credentials are
not stored in this repository.

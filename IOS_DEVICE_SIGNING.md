# iOS device IPA signing setup

The verified GitHub workflow builds a Simulator `.app`. A device-installable
IPA is a different product: Apple requires every executable in the app bundle
to be signed by a developer team and covered by a provisioning profile.

## Required Apple Developer assets

Create these in the Apple Developer portal for the team that will own this app:

1. An Apple Distribution certificate, exported as a password-protected `.p12`.
2. Distribution provisioning profiles for the main app and each extension.
3. An `ExportOptions.plist` that maps each bundle identifier to its profile.

The current project contains these bundle identifiers:

| Target | Current identifier |
| --- | --- |
| Main app | `com.savoirfairelinux.ring` |
| Notification extension | `com.savoirfairelinux.ring.jamiNotificationExtension` |
| Share extension | `com.savoirfairelinux.ring.jamiShareExtension` |

These are upstream identifiers. Before release, replace them with identifiers
registered to your own Apple team; the main identifier and both extension
identifiers must share the same prefix.

## Required GitHub repository secrets

Add the following secrets in the public repository's **Settings → Secrets and
variables → Actions**. Never commit any of these files or values.

| Secret | Value |
| --- | --- |
| `IOS_TEAM_ID` | Your ten-character Apple Developer team ID |
| `IOS_SIGNING_CERTIFICATE_BASE64` | Base64 of the distribution `.p12` |
| `IOS_SIGNING_CERTIFICATE_PASSWORD` | Password used to export the `.p12` |
| `IOS_PROVISIONING_PROFILES_ZIP_BASE64` | Base64 of a ZIP containing every required `.mobileprovision` file |
| `IOS_EXPORT_OPTIONS_PLIST_BASE64` | Base64 of the distribution `ExportOptions.plist` |
| `IOS_KEYCHAIN_PASSWORD` | A new random password used only by the temporary CI keychain |

Create base64 values locally. On Windows PowerShell:

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes('certificate.p12'))
[Convert]::ToBase64String([IO.File]::ReadAllBytes('profiles.zip'))
[Convert]::ToBase64String([IO.File]::ReadAllBytes('ExportOptions.plist'))
```

## Required `ExportOptions.plist` mapping

The export options must map all bundle identifiers to their matching profile
names, not just the main application. Example shape:

```xml
<key>provisioningProfiles</key>
<dict>
  <key>com.example.p2pmessenger</key>
  <string>P2P Messenger Distribution</string>
  <key>com.example.p2pmessenger.jamiNotificationExtension</key>
  <string>P2P Messenger Notification Distribution</string>
  <key>com.example.p2pmessenger.jamiShareExtension</key>
  <string>P2P Messenger Share Distribution</string>
</dict>
```

## Security boundary

The current repository is public so it can use public GitHub Actions macOS
runners. GitHub repository secrets remain encrypted and are not exposed in
build logs. Do not paste certificate data, passwords, Apple IDs, recovery
codes, or API keys into chat, source files, commits, or issues.

Once all six secrets exist, tell Codex **“签名 Secrets 已配置”**. The next step
is to create the manual, signed device archive workflow and upload the IPA as
a private workflow artifact.

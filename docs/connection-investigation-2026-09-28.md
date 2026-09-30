# Connection investigation, 2026-09-28

The user reports that connectivity itself now fails. Do not treat the earlier
cloud-to-local relay probe as Android application acceptance.

## Verified observations

- The running Windows executable is the CrossWAN build extracted under
  `D:\.minecraft\ShuangDianLiao-Windows-CrossWAN-Test`.
- The saved Windows profile has assisted mode enabled and uses
  `wss://shuangdianliao-relay.plastic-gojirasaurus.workers.dev`.
- The existing application has a TCP connection to the configured Windows
  system proxy, `127.0.0.1:7897`.
- A separate disposable relay identity connects successfully through the
  system proxy. Probes to both saved relay contacts receive
  `recipient_offline`, repeatedly. No original profile or outbox is modified.
- The Worker `/health` returns HTTP 200 when using the system proxy.
- Local DNS returns addresses inconsistent with Cloudflare's DNS response.
  Even when curl is given the Cloudflare-resolved IPv4 addresses explicitly,
  direct TLS fails without the proxy. Thus fixing local DNS alone is not
  established as a solution.
- These results establish a reachability issue for the direct local route,
  but do not establish the Android device's exact error. No Android device
  is available over ADB.

## Diagnostic command

Build `P2PMessengerRelayClientTest` and set
`P2P_MESSENGER_RELAY_PROFILE_ROOT` to an existing Windows application data
directory. The diagnostic reads only connection metadata and contact public
keys from that profile, creates a separate temporary identity, and sends
non-user-visible `probe` packets. It reports peer identity prefixes and relay
delivery states, not private keys or message content. Use a D-drive temporary
directory when running locally.

## Remaining blocker

Inspecting the user's Cloudflare account for an alternative reachable ingress
was rejected by the browser tool: a saved permission blocks
`https://dash.cloudflare.com`. Do not work around that browser restriction.
After the user changes this permission, inspect available domains and hosting
options and validate direct reachability before changing the app endpoint.
Do not require users to enable a separate proxy as the product solution.

No new product build was made in this investigation. Only the diagnostic test
was rebuilt. Existing successful cross-WAN core test run: 36382702576. It does
not verify Android TLS, UI request handling, or mobile network accessibility.

## Follow-up: application friend flow and reconnect fix

The user clarified that the failure occurs when requesting friendship.
Added a separate test executable using the production MessengerController,
isolated temporary profile, real daemon, and the deployed relay. GitHub
runner sends an encrypted request; the controller persists it, emits its
pending-request signal, accepts it, then exchanges messages with the runner.
Runs 36394398407 and 36394804869 passed (the latter includes reconnect code).
These exercise the application controller, not Android UI or its network.

Fixed two concrete gaps: no liveness deadline or foreground reconnect for
stale mobile WebSockets; and a static request dialog that kept saying queued
even after its contact delivery status changed. Added ping/pong deadlines,
connection deadlines, foreground reconnect, and reactive request status.
The late-peer regression forces a connection restart before delivery and
passed against the public relay. Android version advanced to 0.4.8-test (12).

The user's exact latest failure is still unconfirmed. A question asking the
current popup text is pending. Cloudflare ingress permissions remain blocked.
Do not claim these changes prove the user's Android friend request is fixed.

## 2026-09-30: Android key and assisted identity fixes

- Fixed custom Java class lookup on Qt native threads using
  `QJniEnvironment::findClass`, rather than raw JNI `FindClass`.
- Explicitly use RSA-OAEP SHA-256 with MGF1 SHA-1 for both key wrapping and
  Android Keystore unwrapping. A new wrapped key is persisted only after a
  successful unwrap comparison. Existing missing/unreadable keys are NOT reset.
- The independent relay identity no longer waits for a Jami account/invitation
  when switching modes, naming the account or submitting an assisted request.
- Load the encrypted profile independently of the optional direct account ID.
  Keep relay contacts, pending requests and local history when the direct
  identity changes. Old direct sessions are marked as needing re-pairing.
- Refuse to overwrite a present but unreadable profile. Startup errors now
  open the failure popup instead of silently opening a fresh-account form.
- Added an APK debug-only intent acceptance entry, isolated local test records,
  production MessengerController and real TLS/Keystore operations. PASS requires
  friend acceptance, an incoming message and an authenticated outgoing receipt.

### Evidence and limits

Windows engine-independent profile/name/history tests and encryption/tamper/
identity-persistence tests passed. The Worker unit tests passed (3/3).

ARM64 APK run 36661447625 on a separate GitHub network reported:

```
P2P_ANDROID_TLS= true backend= openssl version= OpenSSL 3.1.8 11 Feb 2025
P2P_ANDROID_VAULT= PASS
```

It then encountered SIGILL in the emulator's ARM native translation while
initializing the optional direct engine; it did NOT deliver a friend request.
The full UI run 36658980511 could not start an ARM emulator on GitHub's Apple
Silicon VM (HVF_UNSUPPORTED). Neither result is Android UI acceptance.

The headless APK acceptance path now explicitly disables the optional direct
engine using `P2P_MESSENGER_DISABLE_DIRECT_ENGINE=1`; normal launches do not.
This proves only auxiliary transport, Keystore and application controller
behavior if it passes, not direct connectivity or on-device UI rendering.

No physical Android device is connected. The user's exact network route to the
workers.dev endpoint and compatibility with any previously unreadable vault
remain unverified. Do not substitute a same-LAN test for this missing evidence.
Browser restrictions on the Cloudflare account remain in force; no alternative
account access has been used to bypass them.

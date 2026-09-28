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

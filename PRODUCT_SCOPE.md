# P2P Messenger product scope

This is a GPLv3+ downstream of Jami for Windows, Android, and iOS. Jami is
used only as the embedded cryptographic and peer-to-peer transport engine. The
application interface, product identity, local data model, and release
packaging are original to P2P Messenger.

## Product commitments

- A new user creates a device-held cryptographic identity. No email, phone
  number, centralized account database, or Jami branding is present.
- Identities can be linked to additional devices and exported as encrypted
  recovery backups.
- One-to-one and group conversations are end-to-end encrypted and exchanged
  only with the participants' devices.
- Conversation history remains in each client's local encrypted data store.
- Files transfer directly between peers. The client must make connection,
  progress, failure, and integrity states clear.

## No-third-party-network policy

The shipped application must never initiate a connection to a third-party
service. This is a product invariant, not an optional privacy setting.

- Disable public DHT bootstrap and proxy lists, including all `*.jami.net`
  defaults.
- Disable TURN, STUN, relay fallback, push notifications, JamiNS/name lookup,
  telemetry, crash reporting, remote configuration, and automatic updates.
- Never include remote images, web views, CDN assets, or analytics SDKs.
- Peer discovery is limited to local-network discovery and an explicit,
  user-approved peer invite (QR code or manually exchanged invite). The invite
  contains the peer address and public identity; it is not resolved through a
  directory or rendezvous service.
- A direct connection that cannot be established must fail locally and explain
  that no relay is permitted. It must not silently fall back to a server.

## Product surface

- Ship an original P2P Messenger interface rather than a rebranded upstream
  client. The interface is shared in behavior across Windows, Android, and
  iOS, with native platform bridges only for the embedded engine.
- The first release includes local identity creation, QR/manual peer invite,
  private chat, group chat, local encrypted history, and direct file transfer.
- The app is free to use and has no paid service, account, advertising, or
  cloud-storage dependency.

## Release verification

For Windows, Android, and iOS verify: account creation, contact invitation,
one-to-one messages, group messages, linked-device sync, persisted local
history after restart, a direct file transfer between two devices, and a
network audit proving no connection is attempted except to the user-approved
peer or a local-network peer.

## License

Distributed downstream source and modifications remain available under GPLv3+
in accordance with the upstream license.

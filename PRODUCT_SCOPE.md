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

## Network and infrastructure policy

The shipped application must never initiate a connection to a third-party
service. This is a product invariant, not an optional privacy setting.

- Disable public DHT bootstrap and proxy lists, including all `*.jami.net`
  defaults, public TURN/STUN services, JamiNS/name lookup, telemetry, crash
  reporting, remote configuration, and automatic updates.
- Never include remote images, web views, CDN assets, or analytics SDKs.
- The product may connect only to a first-party infrastructure endpoint that
  its operator deploys and explicitly configures. It is never silently
  substituted with an upstream or third-party endpoint.
- Peer discovery supports local-network discovery, QR/manual invites, and a
  first-party rendezvous service. Friend requests are authenticated with each
  device identity before a contact is created.
- Conversations and files try direct end-to-end peer connections first. If NAT
  traversal prevents that connection, the client may use a first-party,
  short-lived encrypted relay. The relay stores no chat history or file body,
  and receives only ciphertext and routing metadata needed for delivery.
- Mobile push is optional and only permitted when delivered by infrastructure
  operated for this product; it must contain no message text, file contents, or
  contact data. The first release will work without push while the app is open.

## Product surface

- Ship an original P2P Messenger interface rather than a rebranded upstream
  client. The interface is shared in behavior across Windows, Android, and
  iOS, with native platform bridges only for the embedded engine.
- The first release includes local identity creation, QR/manual and
  first-party-rendezvous friend invitations, private chat, group chat, local
  encrypted history, and direct or relayed encrypted file transfer.
- The app is free to use and has no paid service, account, advertising, or
  cloud-storage dependency.

## Release verification

For Windows, Android, and iOS verify: account creation, contact invitation,
one-to-one messages, group messages, linked-device sync, persisted local
history after restart, a direct and relayed file transfer between two devices,
and a network audit proving no connection is attempted except to the
user-approved peer, a local-network peer, or the explicitly configured
first-party endpoint.

## License

Distributed downstream source and modifications remain available under GPLv3+
in accordance with the upstream license.

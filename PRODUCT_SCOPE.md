# P2P Messenger product scope

This is a GPLv3+ downstream of Jami for Windows, Android, and iOS. It keeps
the upstream distributed protocol, cryptographic identity, end-to-end
encryption, local storage, and direct peer-to-peer transfer model intact.

## Product commitments

- A new user creates a device-held cryptographic identity and Jami ID. No
  email, phone number, or centralized account database is required.
- Identities can be linked to additional devices and exported as encrypted
  recovery backups.
- One-to-one and group conversations remain end-to-end encrypted over the Jami
  distributed network.
- Conversation history remains in each client's local encrypted data store.
- Files transfer directly between peers whenever connectivity permits. The
  client must make connection, progress, failure, and integrity states clear.
- Any relay is a connectivity fallback only; it must not become a message or
  file-history service.

## Release verification

For Windows, Android, and iOS verify: account creation, contact invitation,
one-to-one messages, group messages, linked-device sync, persisted local
history after restart, and a direct file transfer between two devices.

## License

Distributed downstream source and modifications remain available under GPLv3+
in accordance with the upstream license.

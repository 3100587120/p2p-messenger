# First-party infrastructure

This directory deploys the only optional online infrastructure used by P2P
Messenger. It is operated by the product owner, not Jami or any other third
party.

`rendezvous` keeps only an in-memory map of online public-key identities. It
validates a device-signed registration and forwards opaque encrypted envelopes
only to an online recipient. It has no database and intentionally cannot store
messages, files, contacts, or history.

`turn` is a self-hosted coturn instance for the exceptional case where direct
peer-to-peer connectivity is impossible. The application must label this state
as relayed. Conversation payloads and file bytes remain end-to-end encrypted.

## Deploy

On an owner-controlled server, create a `.env` beside `docker-compose.yml`:

```
TURN_REALM=messenger.example.com
TURN_USERNAME=replace-with-a-random-user
TURN_PASSWORD=replace-with-a-long-random-password
```

Then run `docker compose up -d --build`. Expose TCP 8787 through a TLS reverse
proxy as `wss://your-domain/v1/rendezvous`; expose the listed TURN ports
directly. Configure those exact first-party addresses in the product's private
network settings. Do not configure a Jami, Google, Apple, Firebase, public
TURN, or public STUN endpoint.

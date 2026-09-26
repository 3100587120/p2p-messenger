# Self-hosted DHT bootstrap

P2P Messenger does not ship a public bootstrap address. On this Windows
workspace, build OpenDHT's `dhtnode` from the vendored source using
`scripts/build-bootstrap-windows.ps1`. Run it on a machine you control:

```powershell
./dhtnode.exe -d -p 4222
```

Do not pass `-b`: that flag joins another DHT network. Allow inbound UDP 4222
to that machine, then enter its own `host:4222` address in the messenger's
“自建网络设置”. For two clients on the same PC, use `127.0.0.1:4222`. Both
clients must choose the same reachable private bootstrap node.

This node provides discovery only. It does not store chat history or act as a
TURN relay. If direct ICE connectivity fails, run a TURN service that you
control and enter its host, port, username and password in the same settings
dialog. No TURN endpoint is enabled by default.

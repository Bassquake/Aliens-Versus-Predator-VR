# avp-relay

Internet relay for AvP multiplayer. A host opens a room and gets a 5-character code
(for example `HVSN7`); players join with that code. The relay only forwards packets
between a room's host and its players, so it works behind any home router and needs
no port forwarding on the players' side. The protocol is in `source/src/relay_proto.h`.

## Build

```bash
cc -O2 -o avp-relay relay.c                          # Linux
gcc -O2 -o avp-relay.exe relay.c -lws2_32            # Windows (MinGW), for local testing
```

## Run

```bash
./avp-relay            # UDP 27017
./avp-relay 30000      # another port
```

It logs room opens, joins, leaves and timeouts to stdout. Open **UDP 27017** (or the
port you chose) in the server's firewall, and in the provider's firewall if it has one.

## Run as a service (systemd)

```ini
# /etc/systemd/system/avp-relay.service
[Unit]
Description=AvP multiplayer relay
After=network-online.target

[Service]
ExecStart=/usr/local/bin/avp-relay
Restart=always
DynamicUser=yes

[Install]
WantedBy=multi-user.target
```

```bash
sudo cp avp-relay /usr/local/bin/
sudo systemctl enable --now avp-relay
journalctl -u avp-relay -f
```

## Limits

256 rooms and 8 players per room. Rooms and players expire after 60 s of silence. A
restart drops games in progress; each host gets its code back within 10 s, so players
can rejoin with the code they already have.

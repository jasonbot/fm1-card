# Network API

The device answers a small HTTP API while Wi-Fi
is connected. There is no authentication; treat it as a trusted-LAN service.

## Discovery

- UDP broadcast on port **8135**: send `hello` (or `FM1CARD?`).
- Reply: `fm1-card;<ip>;80`
- HTTP API then listens at `http://<ip>/`.

Example:

```sh
printf 'hello' | socat - UDP-DATAGRAM:255.255.255.255:8135,broadcast
```

There is deliberately no mDNS dependency: the 512 KB S3 needs the heap for
the HTTP server and TLS downloads.

## `GET /status`

Returns current state:

```json
{
  "app": "fm1-card",
  "version": "0.1.1",
  "sd": true,
  "wifi": "connected",
  "devices": [
    {"slot": 0, "path": "h0p1", "identity": "FM-1_916", "clock": false}
  ],
  "msc": 0,
  "clock": {"bpm": 120, "running": false},
  "library": {"catalog": 746, "local": 64, "local_total": 746},
  "hoard": false
}
```

## `GET /firmwares`

Streams a JSON array of files in `fm1-card/firmwares` and `firmwares`:

```json
[
  {"name":"felucca-1.8.3.fwsc","dir":"fm1-card/firmwares","size":610286,
   "sha256":"..."}
]
```

`sha256` comes from the `.fwsc.meta` sidecar and is `null` when no sidecar
exists.

## `POST /firmwares?name=<file>.fwsc`

Uploads a raw `.fwsc` body. The name must end in `.fwsc`; path separators are
rejected. The device writes a temporary file, validates the `.fwsc` identity
layout, replaces an existing file of the same name, and creates a `.meta`
sidecar with the local SHA-256.

```sh
curl -X POST --data-binary @sloop-2.8.5.fwsc \
  'http://fm1-card.local/firmwares?name=sloop-2.8.5.fwsc'
```

Response:

```json
{"ok":true,"name":"sloop-2.8.5.fwsc","product":"FM-1_900","size":610076}
```

The UI refreshes its library after an upload.

## CORS

`/firmwares` and `/status` send `Access-Control-Allow-Origin: *`, and
`OPTIONS /firmwares` answers preflight. A native desktop app does not need
CORS; it is only for browser-based frontends.

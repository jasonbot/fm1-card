# Network API

UDP discovery on port `8135` replies `fm1-card;<ip>;80;<device-name>` to
`hello`. The first three fields are the original format; the fourth is the
editable device name.

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
  "version": "0.1.2",
  "name": "Cardputer",
  "sd": true,
  "wifi": "connected",
  "ssid": "scheirer-eero",
  "rssi": -78,
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

Streams a JSON array of files in `fm1-card/firmwares` and `firmwares`.
By default it returns names only, which is fast even with hundreds of files:

```json
[
  {"name":"felucca-1.8.3.fwsc","dir":"fm1-card/firmwares"}
]
```

Add `?detail=1` to include `size` and the sidecar `sha256` (null when no
sidecar exists). This requires a stat/meta pass over every file and is much
slower on SD.

## `POST /catalog`

Accepts a JSON firmware manifest in the same shape as the zoo feed: either a
bare array or `{"firmwares":[...]}`. The device writes it to
`fm1-card/firmwares.json` and atomically rebuilds `fm1-card/catalog.bin`.

```sh
curl --data-binary @firmwares.json http://<device>/catalog
# {"ok":true,"entries":817}
```

Malformed JSON, an empty manifest, or a body over 4 MiB returns HTTP 400 and
leaves the previous catalog untouched. `L` in the LIB pane performs the same
import from a manifest already on the SD card.

## `GET /firmwares/info?name=<file>.fwsc`

Returns metadata for one installed file, computing the SHA-256 on demand even
when no sidecar exists:

```json
{"name":"sloop-2.8.5.fwsc","dir":"fm1-card/firmwares","size":610076,
 "sha256":"b044...","product":"FM-1_900","device":"fm1","variant":"all"}
```

This is the reliable duplicate check: hash the local file and compare. Listing
without `detail=1` does not include hashes.

## `POST /firmwares?name=<file>.fwsc`

Optional display metadata can accompany the upload:
`title`, `author`, `version`, `description`, `tags`. These are persisted in
the package's `.meta` sidecar. If the file matches a catalog entry by SHA-256,
filename, or catalog package path, the catalog metadata is used automatically.

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

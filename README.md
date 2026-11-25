# fm1-card

A standalone USB-MIDI host, MIDI router and firmware manager for the
**M5Stack Cardputer-Adv** (ESP32-S3FN8, 8 MB flash, no PSRAM).

It is the handheld sibling of [`fm1-tty`](../fm1-grid/fm1-tty): same firmware
catalog, same M-VAVE FM-1 update protocol, but it drives the USB bus itself
instead of asking a desktop to do it.

```
                 USB-C (host)
                      |
              self-powered USB 2.0 hub
              /       |        \
         FM-1      SMK-37     USB-MIDI keyboard
```

## What it does

- **Uses a local firmware manifest** (`fm1-card/firmwares.json`) as the
  metadata source, loaded at boot and after REST uploads, and downloads
  package URLs from it. Works fully offline from `*.fwsc` files on the card.
  Packages are validated by their `.fwsc` identity and size.
- **Flashes FM-1 / SMK-37 units** in normal mode (USB-MIDI two-phase update),
  rescue mode (the same update command to an unidentified MIDI port), and
  download mode (soft-reboot into the JieLi ROM, then install the open
  rescue bootloader from a full-flash backup).
- **Routes MIDI**: any USB-MIDI source to any combination of USB-MIDI sinks.
- **Runs a MIDI clock**: 20–300 BPM, 24 PPQN, start/stop, to every selected
  device.
- **Works from SD first**: catalog cache, packages, backups and config all
  live on the card. Wi-Fi is optional and can be configured on-device.

The UI is a 240×135 text console driven by the 56-key keyboard. No touch, no
LVGL, no PSRAM. Five tabs: **LIB** (catalog/SD packages), **DEV** (USB devices), **SYS** (SD, Wi-Fi, IP, signal, battery, on-device Wi-Fi setup),
**ROUTE** (MIDI graph) and **CLOCK**. Clock and routing are last because they
are configured least often.

## Status

This repository is the initial implementation. The portable protocol/core
layer is unit-tested on the host against the real 2026-11-22 firmware-zoo
catalog (746 entries). The firmware builds with ESP-IDF 6.1, has been flashed
to a Cardputer-Adv, boots with the display and keyboard, and successfully
claims the USB OTG port in host mode. Remaining bring-up: hub power, real
MIDI-device enumeration/routing, and flashing an FM-1 — see `docs/hardware.md`
for the power finding that matters before you plug anything in.

## Network API

While Wi-Fi is connected, the device answers UDP discovery on port `8135`
(`hello` → `fm1-card;<ip>;80;<device-name>`). It exposes an HTTP API at that address:

- `GET /status` — device, USB-MIDI, clock and library state
- `GET /firmwares` — installed package names; `?detail=1` adds size, SHA-256, title/author/version/tags
- `GET /firmwares/info?name=...` — size, hash, product, device/variant for one file
- `POST /firmwares?name=<file>.fwsc` — upload/overwrite a package; optional
  `title`, `author`, `version`, `description`, `tags` metadata is persisted
- `GET /catalog` — current manifest size, SHA-256, entry count and mtime
- `POST /theme?color=RRGGBB` — set and persist the blue accent color
- `POST /catalog` — upload a JSON firmware manifest; atomically rebuilds the
  catalog cache and saves it as `fm1-card/firmwares.json`

See [`docs/api.md`](docs/api.md) for examples. CORS is enabled for browser
frontends; native apps can ignore it.

## Releases

Prebuilt images are published at
[github.com/jasonbot/fm1-card/releases](https://github.com/jasonbot/fm1-card/releases).
Each release has a merged full-flash image plus the app, bootloader, partition
table and `SHA256SUMS`. Tags matching `v*` build and publish automatically via
`.github/workflows/release.yml`.

## Hardware

- M5Stack Cardputer-Adv (Stamp-S3A, ESP32-S3FN8, 8 MB flash).
- A **self-powered USB 2.0 hub**. The Cardputer's USB-C port has 5.1 kΩ
  pull-downs on CC1/CC2 and does not source VBUS. Do not expect it to power a
  bus-powered device or hub. See `docs/hardware.md`.
- A microSD card (FAT32/exFAT).
- A USB-C to USB-A adapter/cable for the hub's upstream port.

## Build

```sh
. $IDF_PATH/export.sh          # ESP-IDF 6.1.x
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The USB port is a host port in the running application, so the USB serial
console disappears once `fm1-card` starts. Use the display, the SD log, or
the optional Wi-Fi console for diagnostics.

### Flashing and recovery

The Cardputer-Adv enters ROM download mode with the side power switch **OFF**:
hold **G0**, connect USB-C to the computer, then release G0. It should appear
as `/dev/ttyACM0` (USB ID `303a:1001`). From a built tree:

```sh
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/fm1-card.bin
```

Or `idf.py -p /dev/ttyACM0 flash`. The flash is verified by esptool. On this
board, esptool's `--after hard-reset` over USB Serial/JTAG does not reliably
start the application; press the reset button (or power-cycle) once after
flashing. On the first boot, `fm1-card` claims the OTG PHY for host mode and
`/dev/ttyACM0` disappears — that is the host port working, not a crash. To
reflash, repeat the G0 download-mode sequence.

Native tests (no hardware, no ESP-IDF):

```sh
cmake -S tests -B build-tests -G Ninja
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

## Formatting

C++ sources use the repository `.clang-format` (Google style, 100 columns).
Vendored `lwjson` and `tinyusb` trees are excluded.

```sh
scripts/format.sh
```

Set `CLANG_FORMAT=/path/to/clang-format` if the binary is not on `PATH`.

## SD layout

```
/sdcard/
  fm1-card/
    config.json              # Wi-Fi and device name
    firmwares.json           # local catalog manifest (source of truth)
    catalog.bin              # cache built from firmwares.json at boot/upload
    firmwares/*.fwsc         # downloaded or imported packages
    backups/*.bin            # full-flash backups before rescue-bootloader work
    logs/fm1-card.log        # optional
  firmwares/*.fwsc           # also scanned, so you can just copy files here
```

`config.json`:

```json
{
  "device_name": "Cardputer",
  "theme_blue": "#000083",
  "wifi": { "ssid": "your-ssid", "password": "your-password" }
}
```

### Bootstrap the card

```sh
scripts/bootstrap-sd.sh /run/media/$USER/FM1CARD \
  --import ../fm1-grid/fm1-firmware/firmware
```

It creates `fm1-card/{firmwares,backups,logs}`, writes `config.json` and a
short on-card README, and copies any `*.fwsc` from `--import`. Existing
config and packages are kept unless `--force` is given.

The local firmware manifest at `fm1-card/firmwares.json` is the source of
truth (bare array or `{"firmwares":[...]}`). It is loaded automatically at boot.
The same file can be uploaded with:

```sh
curl --data-binary @firmwares.json http://fm1-card/catalog
```

`config.json` is read at boot. You can also configure Wi-Fi on the device:
open the **SYS** pane and press `W`, pick a network, type the password, and
press Enter. The firmware writes `config.json` and reconnects immediately;
no reboot is needed. The header shows `WIFI..` while connecting, `WIFI` when
connected, and `WIFI!` on failure.

For Wi-Fi, either edit `config.json` or rerun with credentials (prefer the
environment/prompt over the command-line flag so the password is not in your
shell history):

```sh
FM1_WIFI_SSID='your-ssid' FM1_WIFI_PASSWORD='your-password' \
  scripts/bootstrap-sd.sh /run/media/$USER/FM1CARD --force
```

## Keys

The footer always shows the numbered actions for the current screen. Press the
number to run it. There are no mnemonic shortcuts.

- **Devices**: `↑`/`↓` select; `1 CLOCK` toggles the clock target,
  `2 FLASH` opens the local package picker, `3 DOWN` sends download mode.
  On a ROM disk, `1 ROM` opens Install-firmware / Backup /
  Install-rescue-bootloader. `Enter` on a live MIDI device opens the flash
  picker.
- **Library**: `1 DEVICE` and `2 CHIP` cycle filters, `3 FILTER` edits the
  text filter, `4 FLASH` sends the selected installed package to a connected
  device, and `5 HOARD` downloads everything.
  `Enter` downloads the selected catalog entry. Search is non-interactive:
  type while the search bar is focused, then press Enter to apply and return
  to the list (Esc clears it).
- **System**: `1 DEBRICK` appears when an FM-1 is in its MIDI update loader
  (`ota-…`), when a `WL82 UBOOT1.00` ROM disk is attached, or when no MIDI
  device is present. On the MIDI loader it writes the official FM-1 image
  directly through the update protocol. On the ROM disk it restores the
  official V15 application from a verified package. The ROM path requires a
  verified V15 package already in the local library; it reads and saves the
  flash twice, refuses a mismatched chip/flash ID, writes only
  `0x4000..0x92FFF`, and verifies readback. The action menu keeps `2 WI-FI`
  and `3 NAME` when DEBRICK is available; otherwise they are `1 WI-FI` and
  `2 NAME`.
- **Routes**: `1 ROUTE` opens the sink picker; `2 CLEAR` clears all links.
- **Clock**: `1 RUN` starts/stops, `2 ALL` toggles all targets,
  `3 -BPM` / `4 +BPM` adjust tempo, `5 TARGET` toggles the selected device.
- **Flash**: in the picker, `↑`/`↓` move one row; `Ctrl`+`↑`/`↓` skip five
  rows; `Enter` flashes; `Esc` goes back. The picker loads 32 local packages
  per window and shows its page in the title. Local packages must be on the
  card first: select a catalog entry in LIB and press Enter to download it.
- The Cardputer has no dedicated arrow keys: bare `;` `,` `.` `/` move
  up/left/down/right on list screens; hold `Fn` for the same keys inside text
  fields.
- `Tab` cycles tabs forward; `Ctrl`+`Tab` cycles backward.

## Layout

```
components/fm1_core/       portable C++23: MIDI packets, packages, update
                            server, router, clock, WL82 loader, flash analysis
components/fm1_platform/   ESP-IDF adapters: TinyUSB host, SD, Wi-Fi, HTTP,
                            display, TCA8418 keyboard, flash worker
components/fm1_ui/         240×135 4-bpp canvas and console widgets
components/lwjson/         vendored lwjson 1.10 (MIT)
components/tinyusb/        vendored TinyUSB host stack (MIT)
main/                      boot, task wiring, app state machine
tests/                     native unit tests for fm1_core + catalog parser
docs/                      hardware, architecture, flashing notes
data/                      bundled MIT rescue bootloaders and loader
```

## License

GPL-3.0-or-later. The protocol/package knowledge is ported from `fm1-grid`
(GPL-3.0); the bundled bootloader assets are MIT (see `data/`). TinyUSB and
lwjson keep their own MIT licenses.

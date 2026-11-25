# Flashing notes

Two different chips, two different protocols. Keep them separate in your head:

| Target | Transport | Tool in this project |
|---|---|---|
| FM-1 / SMK-37 **application** | USB-MIDI SysEx | normal / rescue flash |
| FM-1 / SMK-37 **bootloader** | JieLi ROM USB disk (MSC + SCSI) | download mode + rescue bootloader |

## Package format (`*.fwsc`)

The first 20 blocks are 0x30 bytes; byte 0x2F of each block is an identity
marker. Stock SMK-37 v016 uses 36 blocks. The marker byte is
`identity_char + index + 1`, or `0x7D` for padding. `product_of()` recovers
`MODEL_VERSION`; `logical_image()` returns the bytes without the marker
bytes. That logical image is what the device pulls during an update, and its
offsets are what the device requests.

The package is read in place from SD: each 512-byte request is mapped through
the 0x30/0x2F interleave. There is no 1 MiB temporary file.

## Normal update protocol

Constants:

```
HS_QUERY      F0 00 32 45 00 00 00 40 7F F7   identity query
UPGRADE       F0 22 24 35 7F F7               start pulling the image
DOWNLOAD_MODE F0 22 24 35 7D F7               reboot to ROM disk
FINISH_STAGE  E0000000                        step 1 complete
FINISH_WRITE  F0000000                        step 2 complete
```

All update data is 7-bit packed. A request from the device is a SysEx whose
payload is `00 59 30 ...`; the server answers with `00 59 30 <len> <flag>
<addr32> <len16> 0 <data> <checksum>`. The desktop implementation sleeps
10 ms before each answer; keep that pacing, the flash loader is slow.

Flow:

1. Probe identity. Refuse if the package model does not fit the running
   model/version (`identical model`, or the documented SMK-37 custom-firmware
   cross-flash rule).
2. Send `UPGRADE`; wait 2 s.
3. Serve until `FINISH_STAGE`; idle timeout 8 s. If the device goes quiet
   without the finish address, report the request count and stop. On a newer
   FM-1 with the UC25HQ80 flash, the running firmware may not support that
   chip.
4. The unit reboots into `ota-<model>_<version>` on the same hub port. Wait
   up to 90 s. The port path (`hub_addr`, `hub_port`), not the USB address,
   is the key.
5. Send `UPGRADE` again; serve until `FINISH_WRITE` (up to 180 s).
6. The unit reboots into the new firmware. Wait up to 60 s, then compare the
   handshake identity with the package's `MODEL_VERSION`.

If step 3 or 5 stalls, the unit stays in its update loader and a retry
resumes from step 5.

## Rescue flash

A unit that no longer answers the handshake still usually exposes a MIDI
port. Send `UPGRADE` to it and serve with both finish addresses. If the port
was in the running firmware, the flow is stage → reboot → write; if it was
already in the loader, the same serve writes directly. No identity check is
possible on an unidentified port, so the package must be chosen carefully.

A unit that identifies itself as `ota-<model>_<version>` is the update loader
itself. The normal flash flow detects that and uses the same direct serve;
`1 DEBRICK` in SYS also offers it using the official FM-1 package already in
the local library. This is the path for a unit that got stuck after a failed
or interrupted OTA update.

## Download mode and the open bootloader

`DOWNLOAD_MODE` is a SysEx command handled by M-VAVE firmware and most custom
builds. After it, the unit re-enumerates as:

```
VID/PID: (JieLi ROM)
INQUIRY vendor : WL82
INQUIRY product: UBOOT1.00
```

The ROM exposes a USB mass-storage device. This project uses TinyUSB MSC and
vendor CDBs; it does not mount a filesystem.

ROM commands (big-endian CDB command word, 16-byte CDB):

```
WRITE_MEMORY FB06  addr32 len16 0 crc16(data)  -> data
JUMP         FB08  addr32 arg16
ERASE_BLOCK  FB00  addr32
ERASE_SECTOR FB01  addr32
WRITE_FLASH  FB04  addr32 len16 0 crc16(data)  -> data
READ_FLASH   FD05  addr32 len16                <- data
READ_KEY     FC09  AC6900
ONLINE_DEVICE FC0A
IDENTIFY     FC40
CRC32        FC41  addr32 len32                -> crc32
BUFFER_SIZE  FC14                              -> max io size
```

The ROM's upload loader (`mflr-loader-a.bin`, 6016 bytes) is uploaded to
`0x01C20000`, jumped to, then identified by `MFLR`. Data is scrambled with a
CRC16-chained keystream seeded from the chip key (`0xFFFFFFFF` for the loader).

Rescue-bootloader install:

1. Full 1 MiB read, CRC32 compared with the chip's own CRC32. Save to
   `/sdcard/fm1-card/backups/`.
2. Analyze boot area: header CRC, file table, `uboot.boot`, `isd_config.ini`,
   chip-key record, app checksum/identity. Board key is `fm1` or `smk37`
   from the bootloader identity, falling back to the flash id
   (`0x856014`/`0xB36014` → fm1, `0xEB6014` → smk37).
3. Build a 16 KiB boot area: keep bytes 0..0xA0, rewrite entry 0 for the new
   `uboot.boot`, keep the chip-key record and append the fixed config
   settings, rewrite entry 1.
4. Erase 0x0000–0x4000 (block erase), write, read back, compare.
5. The unit reboots into m-bootloader; it can then accept a package over its
   normal update path.

Refuse to write if the model cannot be identified or the existing table
would be overwritten outside the boot area. The backup is the escape hatch.

Firmware install from the ROM disk:

1. Parse the selected `.fwsc` and its UFW `flash.bin` entry.
2. Read the unit's boot area and identify its board/model; reject a package
   for the wrong model.
3. Save the full flash backup, then erase and write only
   `0x4000..0x92FFF` from the package.
4. Read back every sector and compare the application-region hash before
   reporting success.

`1 ROM` opens this as **Install firmware**, alongside Backup and Install
rescue bootloader.


## Software DEBRICK

`1 DEBRICK` in SYS has two paths:

- **MIDI update loader**: a unit answering `ota-<model>_<version>` is served
  the official FM-1 image over the normal update protocol. It needs no ROM
  disk. The action uses V15 when installed, otherwise V16.
- **WL82 ROM disk**: a unit exposing an MSC device with vendor `WL82` and
  product `UBOOT1.00` is restored from the official `FM-1_015` package. The
  worker reads the whole 1 MiB flash twice, saves a verified backup, re-reads
  before writing, erases only `0x4000..0x92FFF`, writes and reads back each
  sector, then verifies the V15 firmware-region SHA-256. Chip key `0x980F`
  and flash IDs `0x856014`/`0xB36014` are mandatory.

The MIDI loader path takes priority when both kinds of target are attached.

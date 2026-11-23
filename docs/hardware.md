# Hardware notes — Cardputer-Adv as a USB host

Sources: M5Stack Cardputer-Adv docs (SKU K132-Adv) and schematic
`Sch_M5CardputerAdv_v1.0_2025_06_20_17_19_58.pdf`.

## SoC

- ESP32-S3FN8: dual-core LX7 @ 240 MHz, 512 KB SRAM, **8 MB flash, no PSRAM**.
- Wi-Fi 2.4 GHz + BLE 5.0.
- One USB OTG 1.1 (full-speed) controller on GPIO19/GPIO20.
- The same port is used for flashing (ROM USB Serial/JTAG) and, at runtime,
  for host mode. Host and device roles are mutually exclusive.

## Pin map (used by this project)

| Function | GPIO | Notes |
|---|---:|---|
| USB D− / D+ | 19 / 20 | internal PHY, TinyUSB host |
| LCD SCLK | 36 | ST7789V2 |
| LCD MOSI | 35 | |
| LCD CS | 37 | |
| LCD DC (RS) | 34 | |
| LCD RST | 33 | |
| LCD BL / RGB power | 38 | high = on; also gates the RGB LED |
| Keyboard SDA | 8 | TCA8418 @ 0x34 |
| Keyboard SCL | 9 | |
| Keyboard INT | 11 | not required; this firmware polls the event FIFO |
| SD CS | 12 | SPI |
| SD MOSI | 14 | |
| SD CLK | 40 | |
| SD MISO | 39 | |
| Battery ADC | 10 | |
| Grove | 1 / 2 | unused |
| EXT UART | 13 / 15 | optional debug console |
| IR TX | 44 | unused |

Display: ST7789V2, 240×135, landscape. The panel is a 240×320 controller;
the visible window needs the usual 40/53 offsets. Verify orientation on the
first unit.

Keyboard: TCA8418 7×8 matrix, 56 keys. The raw event (`1..80`) is remapped to
the Cardputer's 4×14 logical matrix exactly as M5's `TCA8418KeyboardReader`
does: `col = row*2 + (col>3)`, `row = (col+4)%4`.

## Power: the important part

The Cardputer-Adv **cannot source VBUS on its USB-C port**:

- The Stamp-S3A's USB-C connector has **5.1 kΩ pull-downs on CC1 and CC2**
  (R1/R2 in the Stamp-S3A schematic), i.e. it is wired as a sink/device.
- VBUS from the connector is `+5VIN`, which feeds the TP4057 charger and the
  SY7088 boost. The boost produces `+5VOUT` for the board and the EXT header;
  there is no switch or path from `+5VOUT` back to the USB-C VBUS pin.
- Therefore a bus-powered hub or a bus-powered MIDI device will not be
  powered by the Cardputer.

**Use a self-powered USB 2.0 hub.** The Cardputer only needs D+/D− and GND
for host signalling. If the hub insists on seeing upstream VBUS before it
enables its downstream ports, inject 5 V into the upstream connector's VBUS
from the EXT header's `5VOUT`/GND through a Schottky diode (never direct),
or use a powered hub that does not require upstream VBUS. A USB power meter
in line is the quickest way to confirm what the hub expects.

Consequences for the design:

- The firmware never assumes it can power a device; there is no VBUS switch
  or over-current input to control.
- Device disconnect is detected by TinyUSB enumeration, not by VBUS.
- Battery runtime is dominated by the hub and the MIDI devices, not the
  Cardputer.

## USB host topology

- TinyUSB host, full speed, with `CFG_TUH_HUB=1`.
- Up to 7 downstream devices with the default 8-address budget.
- MIDI and MSC class drivers run simultaneously: a normal FM-1 is a MIDI
  device; after `DOWNLOAD MODE` it re-enumerates as the JieLi `WL82 /
  UBOOT1.00` USB mass-storage device.
- USB-MIDI devices that expose several interfaces/cables are treated as one
  logical port per interface; routing uses cable 0 on the sink.

## SD card

SPI mode on SPI3 (G40/39/14/12). FATFS mount at `/sdcard`. Keep the card
FAT32 or exFAT. The catalog cache is ~450 KB (746 entries); packages are ~600 KB each;
backups are 1 MiB each.

## Wi-Fi

The ESP32-S3 has no PSRAM, so TLS + HTTP + the UI must share ~300 KB of free
heap. The catalog is parsed as a stream (lwjson) and written directly to SD;
it is never held in RAM. Wi-Fi is powered down between operations.

## Audio / IMU / IR

Not used. The ES8311 codec, BMI270 and IR emitter are left alone; the I²C bus
is shared with the keyboard, so don't reconfigure those addresses.

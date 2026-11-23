#pragma once

/* Cardputer-Adv runs host-only on its single USB-C port. */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 0

/* One external hub, up to 7 downstream devices. */
#define CFG_TUH_HUB 1
#define CFG_TUH_DEVICE_MAX (3 * CFG_TUH_HUB + 1)

/* MIDI: one interface instance per possible device. */
#define CFG_TUH_MIDI CFG_TUH_DEVICE_MAX
#define CFG_TUH_MIDI_RX_BUFSIZE 1024
#define CFG_TUH_MIDI_TX_BUFSIZE 2048
#define CFG_TUH_MIDI_EP_BUFSIZE 64

/* MSC: only the JieLi ROM disk, one LUN. */
#define CFG_TUH_MSC 1
#define CFG_TUH_MSC_MAXLUN 1

/* No device-mode buffers. */
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#define CFG_TUH_ENUMERATION_BUFSIZE 512
#define CFG_TUSB_DEBUG 0

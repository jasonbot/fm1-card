#pragma once

// TinyUSB host manager. All host API calls happen on one FreeRTOS task; other
// tasks post TX packets and MSC requests through queues.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fm1/midi.hpp"
#include "fm1/result.hpp"
#include "fm1/routing.hpp"

extern "C" {
#include "host/usbh.h"
#include "class/midi/midi_host.h"
#include "class/msc/msc_host.h"
}

namespace fm1::platform {

struct MidiPort {
  uint8_t slot = 0;
  std::string path;      // "h<hub>p<port>", stable across a reboot
  std::string identity;  // MODEL_VERSION from the handshake, if it answered
  bool present = false;
  bool claimed = false;
  bool ota = false;
};

struct MscPort {
  uint8_t slot = 0;
  std::string path;
  std::string vendor;
  std::string product;
  bool present = false;
};

class UsbHost {
 public:
  using EventFn = std::function<void()>;

  static UsbHost& instance();

  Result<void> start();
  void set_event_callback(EventFn callback);

  [[nodiscard]] std::vector<MidiPort> midi_ports() const;
  [[nodiscard]] std::vector<MscPort> msc_ports() const;
  [[nodiscard]] std::optional<MidiPort> midi_port(uint8_t slot) const;
  [[nodiscard]] std::optional<MscPort> msc_port(uint8_t slot) const;

  // Queues packets for the USB task. False when the slot is not mounted.
  bool send_packets(uint8_t slot, std::span<const fm1::midi::Packet> packets);
  bool send_sysex(uint8_t slot, std::span<const uint8_t> sysex);
  std::optional<std::vector<uint8_t>> read_sysex(uint8_t slot, std::chrono::milliseconds timeout);
  void drain_sysex(uint8_t slot);

  void claim(uint8_t slot, bool claimed);
  [[nodiscard]] bool claimed(uint8_t slot) const;

  void set_router(fm1::routing::Router* router);
  void set_clock_targets(fm1::routing::Mask mask);
  void mute(uint8_t slot, bool muted);

  // Polls until a MIDI port on `path` satisfies pred, or the timeout expires.
  std::optional<MidiPort> wait_for_midi(std::string_view path, std::chrono::milliseconds timeout,
                                        const std::function<bool(const MidiPort&)>& pred);

  // MSC SCSI command; data-in uses `in`, data-out uses `out`.
  bool scsi_command(uint8_t slot, const msc_cbw_t& cbw, std::span<uint8_t> in,
                    std::span<const uint8_t> out, std::chrono::milliseconds timeout);

  // Called from the USB task only.
  void task_loop();

 private:
  UsbHost() = default;
  ~UsbHost();

 public:
  // TinyUSB callback entry points; only the USB task calls these.
  void on_midi_mount(uint8_t idx, const tuh_midi_mount_cb_t& mount);
  void on_midi_umount(uint8_t idx);
  void on_midi_rx(uint8_t idx, uint32_t bytes);
  void on_midi_tx(uint8_t idx, uint32_t bytes);
  void on_msc_mount(uint8_t daddr);
  void on_msc_umount(uint8_t daddr);

 private:
  void notify();
  void enqueue(uint8_t slot, const fm1::midi::Packet& packet);
  void drain_tx();
  void drain_msc();
  void probe_loop();
  void notify_probe(uint8_t slot);

  struct State;
  State* state_ = nullptr;
  void* usb_task_ = nullptr;
  void* probe_task_ = nullptr;
  void* probe_queue_ = nullptr;
};

}  // namespace fm1::platform

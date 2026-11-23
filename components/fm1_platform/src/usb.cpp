#include "fm1/platform/usb.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <semaphore>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "class/midi/midi_host.h"
#include "host/usbh.h"
#include "tusb.h"

namespace fm1::platform {
namespace {

constexpr const char* kTag = "fm1-usb";
constexpr uint8_t kMaxMidi = CFG_TUH_MIDI;
constexpr uint8_t kMaxMsc = 4;

struct DmaBuffer {
  explicit DmaBuffer(std::size_t size) {
    if (size == 0) size = 1;
    ptr = heap_caps_aligned_alloc(4, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    length = ptr != nullptr ? size : 0;
  }
  ~DmaBuffer() {
    if (ptr != nullptr) heap_caps_free(ptr);
  }
  DmaBuffer(const DmaBuffer&) = delete;
  DmaBuffer& operator=(const DmaBuffer&) = delete;
  void* ptr = nullptr;
  std::size_t length = 0;
};

struct MscRequest {
  msc_cbw_t cbw{};
  uint8_t daddr = 0;
  DmaBuffer buffer;
  bool data_in = false;
  uint32_t residue = 0;
  std::binary_semaphore done{0};
  volatile bool ok = false;
  MscRequest(std::size_t size, bool in) : buffer(size), data_in(in) {}
};

}  // namespace

struct UsbHost::State {
  struct Midi {
    std::mutex mutex;
    std::condition_variable changed;
    bool present = false;
    uint8_t daddr = 0;
    uint8_t hub_addr = 0;
    uint8_t hub_port = 0;
    std::string path;
    std::string identity;
    bool claimed = false;
    bool muted = false;
    bool ota = false;
    std::deque<fm1::midi::Packet> packets;
    std::deque<std::vector<uint8_t>> sysex;
    fm1::midi::SysexAssembler assembler;
  } midi[kMaxMidi];

  struct Msc {
    std::mutex mutex;
    bool present = false;
    uint8_t daddr = 0;
    uint8_t hub_addr = 0;
    uint8_t hub_port = 0;
    std::string path;
    std::string vendor;
    std::string product;
  } msc[kMaxMsc];

  std::mutex tx_mutex;
  std::deque<std::pair<uint8_t, fm1::midi::Packet>> tx;

  std::mutex msc_mutex;
  std::deque<std::shared_ptr<MscRequest>> msc_queue;

  std::mutex router_mutex;
  fm1::routing::Router* router = nullptr;
  fm1::routing::Mask clock_targets = 0;

  std::mutex event_mutex;
  EventFn event;
};

UsbHost& UsbHost::instance() {
  static UsbHost host;
  return host;
}

UsbHost::~UsbHost() = default;

Result<void> UsbHost::start() {
  state_ = new State();
  probe_queue_ = xQueueCreate(16, sizeof(uint8_t));
  if (probe_queue_ == nullptr) return fail("no probe queue");

  tusb_rhport_init_t init{};
  init.role = TUSB_ROLE_HOST;
  init.speed = TUSB_SPEED_FULL;
  if (!tusb_init(0, &init)) {
    return fail("TinyUSB host init failed");
  }
  TaskHandle_t usb_handle = nullptr;
  xTaskCreatePinnedToCore(
      [](void* arg) {
        static_cast<UsbHost*>(arg)->task_loop();
      },
      "fm1-usb", 8192, this, 5, &usb_handle, 0);
  usb_task_ = usb_handle;
  TaskHandle_t probe_handle = nullptr;
  xTaskCreatePinnedToCore(
      [](void* arg) {
        static_cast<UsbHost*>(arg)->probe_loop();
      },
      "fm1-probe", 4096, this, 4, &probe_handle, 0);
  probe_task_ = probe_handle;
  return {};
}

void UsbHost::set_event_callback(EventFn callback) {
  std::lock_guard lock(state_->event_mutex);
  state_->event = std::move(callback);
}

void UsbHost::notify() {
  EventFn callback;
  {
    std::lock_guard lock(state_->event_mutex);
    callback = state_->event;
  }
  if (callback) callback();
}

std::string make_path(uint8_t hub_addr, uint8_t hub_port) {
  return "h" + std::to_string(hub_addr) + "p" + std::to_string(hub_port);
}

void UsbHost::on_midi_mount(uint8_t idx, const tuh_midi_mount_cb_t& mount) {
  if (idx >= kMaxMidi) return;
  tuh_bus_info_t bus{};
  tuh_bus_info_get(mount.daddr, &bus);
  auto& port = state_->midi[idx];
  {
    std::lock_guard lock(port.mutex);
    port.present = true;
    port.daddr = mount.daddr;
    port.hub_addr = bus.hub_addr;
    port.hub_port = bus.hub_port;
    port.path = make_path(bus.hub_addr, bus.hub_port);
    port.identity.clear();
    port.ota = false;
    port.claimed = false;
    port.muted = false;
    port.packets.clear();
    port.sysex.clear();
  }
  send_sysex(idx, fm1::midi::kHandshakeQuery);
  notify_probe(idx);
  notify();
  ESP_LOGI(kTag, "MIDI %u at %s", idx, make_path(bus.hub_addr, bus.hub_port).c_str());
}

void UsbHost::on_midi_umount(uint8_t idx) {
  if (idx >= kMaxMidi) return;
  auto& port = state_->midi[idx];
  {
    std::lock_guard lock(port.mutex);
    port.present = false;
    port.identity.clear();
    port.ota = false;
    port.claimed = false;
    port.packets.clear();
    port.sysex.clear();
    port.changed.notify_all();
  }
  notify();
}

void UsbHost::on_midi_rx(uint8_t idx, uint32_t) {
  if (idx >= kMaxMidi) return;
  std::array<uint8_t, 256> buffer{};
  for (;;) {
    const uint32_t got = tuh_midi_packet_read_n(idx, buffer.data(), buffer.size());
    if (got < 4) break;
    for (uint32_t at = 0; at + 4 <= got; at += 4) {
      const fm1::midi::Packet packet{buffer[at], buffer[at + 1], buffer[at + 2], buffer[at + 3]};
      std::optional<std::vector<uint8_t>> completed;
      bool claimed = false;
      bool muted = false;
      {
        auto& port = state_->midi[idx];
        std::lock_guard lock(port.mutex);
        if (!port.present) continue;
        port.packets.push_back(packet);
        if (port.packets.size() > 512) port.packets.pop_front();
        completed = port.assembler.feed(packet);
        claimed = port.claimed;
        muted = port.muted;
        if (completed) {
          port.sysex.push_back(*completed);
          if (port.sysex.size() > 16) port.sysex.pop_front();
          port.changed.notify_all();
        }
      }
      if (!claimed && !muted) {
        fm1::routing::Mask sinks = 0;
        {
          std::lock_guard lock(state_->router_mutex);
          if (state_->router != nullptr) sinks = state_->router->sinks(idx);
        }
        for (uint8_t sink = 0; sink < fm1::routing::kMaxPorts; ++sink) {
          if ((sinks & (1u << sink)) != 0) enqueue(sink, packet);
        }
      }
    }
  }
}

void UsbHost::on_midi_tx(uint8_t, uint32_t) {}

void UsbHost::on_msc_mount(uint8_t daddr) {
  tuh_bus_info_t bus{};
  tuh_bus_info_get(daddr, &bus);
  for (uint8_t slot = 0; slot < kMaxMsc; ++slot) {
    auto& msc = state_->msc[slot];
    std::lock_guard lock(msc.mutex);
    if (msc.present) continue;
    msc.present = true;
    msc.daddr = daddr;
    msc.hub_addr = bus.hub_addr;
    msc.hub_port = bus.hub_port;
    msc.path = make_path(bus.hub_addr, bus.hub_port);
    msc.vendor.clear();
    msc.product.clear();
    notify();
    ESP_LOGI(kTag, "MSC %u at %s", slot, msc.path.c_str());
    return;
  }
}

void UsbHost::on_msc_umount(uint8_t daddr) {
  for (auto& msc : state_->msc) {
    std::lock_guard lock(msc.mutex);
    if (msc.present && msc.daddr == daddr) {
      msc.present = false;
      msc.vendor.clear();
      msc.product.clear();
    }
  }
  notify();
}

std::vector<MidiPort> UsbHost::midi_ports() const {
  std::vector<MidiPort> out;
  for (uint8_t slot = 0; slot < kMaxMidi; ++slot) {
    auto& port = state_->midi[slot];
    std::lock_guard lock(port.mutex);
    if (!port.present) continue;
    out.push_back(MidiPort{.slot = slot,
                           .path = port.path,
                           .identity = port.identity,
                           .present = true,
                           .claimed = port.claimed,
                           .ota = port.ota});
  }
  return out;
}

std::vector<MscPort> UsbHost::msc_ports() const {
  std::vector<MscPort> out;
  for (uint8_t slot = 0; slot < kMaxMsc; ++slot) {
    auto& msc = state_->msc[slot];
    std::lock_guard lock(msc.mutex);
    if (!msc.present) continue;
    out.push_back(MscPort{.slot = slot,
                          .path = msc.path,
                          .vendor = msc.vendor,
                          .product = msc.product,
                          .present = true});
  }
  return out;
}

std::optional<MidiPort> UsbHost::midi_port(uint8_t slot) const {
  if (slot >= kMaxMidi) return std::nullopt;
  auto& port = state_->midi[slot];
  std::lock_guard lock(port.mutex);
  if (!port.present) return std::nullopt;
  return MidiPort{.slot = slot,
                  .path = port.path,
                  .identity = port.identity,
                  .present = true,
                  .claimed = port.claimed,
                  .ota = port.ota};
}

std::optional<MscPort> UsbHost::msc_port(uint8_t slot) const {
  if (slot >= kMaxMsc) return std::nullopt;
  auto& msc = state_->msc[slot];
  std::lock_guard lock(msc.mutex);
  if (!msc.present) return std::nullopt;
  return MscPort{.slot = slot,
                 .path = msc.path,
                 .vendor = msc.vendor,
                 .product = msc.product,
                 .present = true};
}

void UsbHost::enqueue(uint8_t slot, const fm1::midi::Packet& packet) {
  {
    std::lock_guard lock(state_->tx_mutex);
    if (state_->tx.size() >= 4096) return;
    state_->tx.emplace_back(slot, packet);
  }
  if (usb_task_ != nullptr) {
    xTaskNotifyGive(reinterpret_cast<TaskHandle_t>(usb_task_));
  }
}

bool UsbHost::send_packets(uint8_t slot, std::span<const fm1::midi::Packet> packets) {
  if (slot >= kMaxMidi) return false;
  {
    auto& port = state_->midi[slot];
    std::lock_guard lock(port.mutex);
    if (!port.present) return false;
  }
  for (const auto& packet : packets) enqueue(slot, packet);
  return true;
}

bool UsbHost::send_sysex(uint8_t slot, std::span<const uint8_t> sysex) {
  const auto packets = fm1::midi::encode_message(0, sysex);
  return send_packets(slot, packets);
}

std::optional<std::vector<uint8_t>> UsbHost::read_sysex(uint8_t slot,
                                                        std::chrono::milliseconds timeout) {
  if (slot >= kMaxMidi) return std::nullopt;
  auto& port = state_->midi[slot];
  std::unique_lock lock(port.mutex);
  if (!port.changed.wait_for(lock, timeout, [&] { return !port.sysex.empty(); })) {
    return std::nullopt;
  }
  auto message = std::move(port.sysex.front());
  port.sysex.pop_front();
  return message;
}

void UsbHost::drain_sysex(uint8_t slot) {
  if (slot >= kMaxMidi) return;
  auto& port = state_->midi[slot];
  std::lock_guard lock(port.mutex);
  port.sysex.clear();
}

void UsbHost::claim(uint8_t slot, bool claimed) {
  if (slot >= kMaxMidi) return;
  auto& port = state_->midi[slot];
  std::lock_guard lock(port.mutex);
  port.claimed = claimed;
  if (!claimed) port.changed.notify_all();
}

bool UsbHost::claimed(uint8_t slot) const {
  if (slot >= kMaxMidi) return false;
  auto& port = state_->midi[slot];
  std::lock_guard lock(port.mutex);
  return port.claimed;
}

void UsbHost::set_router(fm1::routing::Router* router) {
  std::lock_guard lock(state_->router_mutex);
  state_->router = router;
}

void UsbHost::set_clock_targets(fm1::routing::Mask mask) {
  std::lock_guard lock(state_->router_mutex);
  state_->clock_targets = mask;
}

void UsbHost::mute(uint8_t slot, bool muted) {
  if (slot >= kMaxMidi) return;
  auto& port = state_->midi[slot];
  std::lock_guard lock(port.mutex);
  port.muted = muted;
}

std::optional<MidiPort> UsbHost::wait_for_midi(
    std::string_view path, std::chrono::milliseconds timeout,
    const std::function<bool(const MidiPort&)>& pred) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& port : midi_ports()) {
      if (port.path == path && (!pred || pred(port))) return port;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return std::nullopt;
}

void UsbHost::drain_tx() {
  std::deque<std::pair<uint8_t, fm1::midi::Packet>> pending;
  {
    std::lock_guard lock(state_->tx_mutex);
    pending.swap(state_->tx);
  }
  std::size_t at = 0;
  while (at < pending.size()) {
    const uint8_t slot = pending[at].first;
    std::vector<uint8_t> bytes;
    bytes.reserve(256 * 4);
    std::size_t end = at;
    while (end < pending.size() && pending[end].first == slot && bytes.size() < 2048 - 4) {
      const auto& packet = pending[end].second;
      bytes.push_back(packet.head);
      bytes.push_back(packet.b0);
      bytes.push_back(packet.b1);
      bytes.push_back(packet.b2);
      ++end;
    }
    if (slot < kMaxMidi) {
      bool present = false;
      {
        auto& port = state_->midi[slot];
        std::lock_guard lock(port.mutex);
        present = port.present;
      }
      if (present && !bytes.empty()) {
        const uint32_t queued = tuh_midi_packet_write_n(slot, bytes.data(), bytes.size());
        if (queued < bytes.size()) {
          // Requeue the tail; the endpoint buffer is full.
          const std::size_t done = queued / 4;
          if (done == 0) {
            std::lock_guard lock(state_->tx_mutex);
            for (std::size_t i = at; i < pending.size(); ++i) {
              state_->tx.push_back(pending[i]);
            }
            return;
          }
          pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(at),
                        pending.begin() + static_cast<std::ptrdiff_t>(at + done));
          at = 0;
          continue;
        }
        tuh_midi_write_flush(slot);
      }
    }
    at = end;
  }
}

bool UsbHost::scsi_command(uint8_t slot, const msc_cbw_t& cbw, std::span<uint8_t> in,
                           std::span<const uint8_t> out,
                           std::chrono::milliseconds timeout) {
  if (slot >= kMaxMsc) return false;
  uint8_t daddr = 0;
  {
    auto& msc = state_->msc[slot];
    std::lock_guard lock(msc.mutex);
    if (!msc.present) return false;
    daddr = msc.daddr;
  }
  const std::size_t length = in.empty() ? out.size() : in.size();
  auto request = std::make_shared<MscRequest>(length, !in.empty());
  if (length > 0 && request->buffer.ptr == nullptr) return false;
  request->daddr = daddr;
  request->cbw = cbw;
  request->cbw.total_bytes = static_cast<uint32_t>(length);
  request->cbw.dir = in.empty() ? 0 : 1;
  request->cbw.lun = 0;
  request->cbw.cmd_len = 16;
  if (!out.empty()) {
    std::memcpy(request->buffer.ptr, out.data(), out.size());
  }
  {
    std::lock_guard lock(state_->msc_mutex);
    state_->msc_queue.push_back(request);
  }
  xTaskNotifyGive(reinterpret_cast<TaskHandle_t>(usb_task_));
  if (!request->done.try_acquire_for(timeout)) {
    return false;
  }
  if (!request->ok) return false;
  if (!in.empty()) {
    std::memcpy(in.data(), request->buffer.ptr, in.size());
  }
  return true;
}

void UsbHost::drain_msc() {
  std::shared_ptr<MscRequest> request;
  {
    std::lock_guard lock(state_->msc_mutex);
    if (state_->msc_queue.empty()) return;
    request = state_->msc_queue.front();
    state_->msc_queue.pop_front();
  }
  const uint8_t daddr = request->daddr;
  if (!tuh_msc_ready(daddr)) {
    request->ok = false;
    request->done.release();
    return;
  }
  auto* holder = new std::shared_ptr<MscRequest>(request);
  const bool queued = tuh_msc_scsi_command(
      daddr, &request->cbw, request->buffer.ptr,
      [](uint8_t, const tuh_msc_complete_data_t* data) {
        auto* held = reinterpret_cast<std::shared_ptr<MscRequest>*>(data->user_arg);
        auto req = *held;
        delete held;
        req->residue = data->csw != nullptr ? data->csw->data_residue : 0;
        req->ok = data->csw != nullptr && data->csw->status == MSC_CSW_STATUS_PASSED &&
                  req->residue == 0;
        req->done.release();
        return true;
      },
      reinterpret_cast<uintptr_t>(holder));
  if (!queued) {
    delete holder;
    request->ok = false;
    request->done.release();
  }
}

void UsbHost::notify_probe(uint8_t slot) {
  if (probe_queue_ != nullptr) {
    xQueueSend(reinterpret_cast<QueueHandle_t>(probe_queue_), &slot, 0);
  }
}

void UsbHost::probe_loop() {
  for (;;) {
    uint8_t slot = 0;
    if (xQueueReceive(reinterpret_cast<QueueHandle_t>(probe_queue_), &slot,
                      portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (slot >= kMaxMidi) continue;
    for (int attempt = 0; attempt < 3; ++attempt) {
      drain_sysex(slot);
      send_sysex(slot, fm1::midi::kHandshakeQuery);
      const auto deadline = esp_timer_get_time() + 1000 * 1000;
      while (esp_timer_get_time() < deadline) {
        auto message = read_sysex(slot, std::chrono::milliseconds(200));
        if (!message) continue;
        auto identity = fm1::midi::parse_identity(*message);
        if (!identity) continue;
        auto& port = state_->midi[slot];
        {
          std::lock_guard lock(port.mutex);
          port.identity = *identity;
          port.ota = identity->starts_with("ota-") || identity->starts_with("OTA-");
          port.changed.notify_all();
        }
        notify();
        ESP_LOGI(kTag, "MIDI %u identity %s", slot, identity->c_str());
        goto next_slot;
      }
    }
  next_slot:
    continue;
  }
}

void UsbHost::task_loop() {
  for (;;) {
    tuh_task_ext(1, false);
    drain_tx();
    drain_msc();
  }
}

}  // namespace fm1::platform

extern "C" {

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t* mount) {
  fm1::platform::UsbHost::instance().on_midi_mount(idx, *mount);
}
void tuh_midi_umount_cb(uint8_t idx) {
  fm1::platform::UsbHost::instance().on_midi_umount(idx);
}
void tuh_midi_rx_cb(uint8_t idx, uint32_t bytes) {
  fm1::platform::UsbHost::instance().on_midi_rx(idx, bytes);
}
void tuh_midi_tx_cb(uint8_t idx, uint32_t bytes) {
  fm1::platform::UsbHost::instance().on_midi_tx(idx, bytes);
}
void tuh_msc_mount_cb(uint8_t daddr) {
  fm1::platform::UsbHost::instance().on_msc_mount(daddr);
}
void tuh_msc_umount_cb(uint8_t daddr) {
  fm1::platform::UsbHost::instance().on_msc_umount(daddr);
}

}  // extern "C"

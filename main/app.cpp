#include "app.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <chrono>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fm1/identity.hpp"

namespace fm1 {
namespace {

constexpr const char* kTag = "fm1-app";

void blit_cb(void* context, int x, int y, int width, int height,
             std::span<const uint16_t> pixels) {
  static_cast<platform::Display*>(context)->blit(x, y, width, height, pixels);
}

std::string label_of(const platform::MidiPort& port) {
  return port.identity.empty() ? ("MIDI " + port.path) : port.identity;
}

}  // namespace

App::App(platform::Display& display, platform::Keyboard& keyboard,
         platform::Storage& storage, platform::Battery& battery)
    : display_(display), keyboard_(keyboard), storage_(storage), battery_(battery),
      library_(storage) {}

void App::run() {
  if (auto r = display_.init(); !r) {
    ESP_LOGE(kTag, "display: %s", r.error().message.c_str());
    return;
  }
  boot_stage("display ok");
  if (auto r = keyboard_.init(); !r) {
    ESP_LOGE(kTag, "keyboard: %s", r.error().message.c_str());
    boot_stage("keyboard failed: " + r.error().message, true);
  }
  if (auto r = battery_.init(); !r) {
    ESP_LOGE(kTag, "battery: %s", r.error().message.c_str());
  }

  // Bring USB host up before SD: a slow/failed card mount must not hide the
  // host port from the rest of the system.
  boot_stage("starting USB host...");
  if (auto r = usb_.start(); !r) {
    ESP_LOGE(kTag, "usb: %s", r.error().message.c_str());
    boot_stage("USB host failed: " + r.error().message, true);
    set_message("USB host failed: " + r.error().message, true);
  } else {
    boot_stage("USB host active");
  }
  usb_.set_router(&router_);
  usb_.set_event_callback([this] { dirty_ = true; });
  net_.set_status_provider([this] {
    std::lock_guard lock(status_lock_);
    return status_json_;
  });
  if (auto r = flash_.start(usb_, storage_); !r) {
    ESP_LOGE(kTag, "flash worker: %s", r.error().message.c_str());
  }

  boot_stage("mounting SD card...");
  if (auto r = storage_.mount(); !r) {
    ESP_LOGW(kTag, "storage: %s", r.error().message.c_str());
    boot_stage("SD mount failed: " + r.error().message, true);
    vTaskDelay(pdMS_TO_TICKS(1500));
    set_message("No SD card mounted: library/downloads unavailable", true, 15'000);
  } else {
    boot_stage("SD card mounted");
    (void)storage_.ensure_dir("fm1-card");
    (void)storage_.ensure_dir("fm1-card/firmwares");
    (void)storage_.ensure_dir("fm1-card/backups");
    (void)storage_.ensure_dir("fm1-card/logs");
    char boot[160]{};
    std::snprintf(boot, sizeof(boot), "reset=%d free=%u largest=%u\n",
                  static_cast<int>(esp_reset_reason()),
                  static_cast<unsigned>(
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    (void)storage_.write_text("fm1-card/logs/boot.log", boot);
  }
  queue_net({.command = NetCommand::Start, .entry = {}, .destination = {}});

  xTaskCreatePinnedToCore([](void* arg) { static_cast<App*>(arg)->ui_loop(); }, "fm1-ui",
                          8192, this, 4, nullptr, 1);
  xTaskCreatePinnedToCore([](void* arg) { static_cast<App*>(arg)->clock_loop(); },
                          "fm1-clock", 4096, this, 5, nullptr, 1);
  xTaskCreatePinnedToCore([](void* arg) { static_cast<App*>(arg)->net_loop(); }, "fm1-net",
                          20480, this, 2, nullptr, 0);
}

void App::set_message(std::string message, bool error, uint32_t ttl_ms) {
  {
    std::lock_guard lock(message_lock_);
    message_ = std::move(message);
    message_is_error_ = error;
    message_until_ = esp_timer_get_time() / 1000 + ttl_ms;
  }
  dirty_ = true;
}

void App::boot_stage(std::string_view stage, bool error) {
  ui_.canvas().clear(0);
  ui_.canvas().fill(0, 0, 240, 8, error ? 6 : 3);
  ui_.canvas().text(0, 0, "FM1-CARD BOOT", 0);
  ui_.canvas().text_clipped(0, 2, 40, stage, error ? 6 : 1);
  ui_.canvas().present(&display_, blit_cb);
}

void App::queue_net(NetJob job) {
  {
    std::lock_guard lock(net_lock_);
    net_jobs_.push_back(std::move(job));
  }
  net_wake_.notify_one();
}

void App::ui_loop() {
  uint64_t last_draw = 0;
  for (;;) {
    while (auto event = keyboard_.poll()) handle_key(*event);

    if (library_dirty_.exchange(false)) refresh_library();

    const uint64_t now = esp_timer_get_time() / 1000;
    if (dirty_.exchange(false) || now - last_draw > 200) {
      sync_ui();
      ui_.draw(state_);
      ui_.canvas().present(&display_, blit_cb);
      last_draw = now;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void App::clock_loop() {
  for (;;) {
    const auto due = clock_.next_due_us();
    if (!due) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    if (now < *due) {
      const uint64_t wait_us = std::min<uint64_t>(*due - now, 2000);
      vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
      continue;
    }
    const auto mask = router_.clock_targets();
    static const std::array<uint8_t, 1> tick_byte{0xF8};
    static const auto tick = fm1::midi::encode_message(0, tick_byte);
    for (const auto& port : usb_.midi_ports()) {
      if ((mask & (1u << port.slot)) != 0 && !port.claimed) {
        usb_.send_packets(port.slot, tick);
      }
    }
    clock_.advance(now);
    if (clock_.tick() % 6 == 0) dirty_ = true;
  }
}

void App::net_loop() {
  for (;;) {
    NetJob job;
    {
      std::unique_lock lock(net_lock_);
      net_wake_.wait(lock, [&] { return !net_jobs_.empty(); });
      job = std::move(net_jobs_.front());
      net_jobs_.pop_front();
    }
    switch (job.command) {
      case NetCommand::Start: {
        auto config = platform::load_config(storage_);
        if (!config) {
          set_message(config.error().message, true);
          break;
        }
        if (config->ssid.empty()) {
          set_message("No Wi-Fi configured: edit fm1-card/config.json, then reboot", false,
                      15'000);
          (void)net_.start(storage_, *config);
          break;
        }
        set_message("Wi-Fi: connecting to \"" + config->ssid + "\"...", false, 30'000);
        auto started = net_.start(storage_, *config);
        if (!started) {
          set_message("Wi-Fi: " + started.error().message, true, 15'000);
        } else if (net_.wifi_connected()) {
          set_message("Wi-Fi connected: press R in LIB to refresh", false, 10'000);
          library_dirty_ = true;
        } else {
          set_message("Wi-Fi: still connecting...", false, 30'000);
        }
        break;
      }
      case NetCommand::Refresh: {
        set_message(net_.wifi_connected() ? "Fetching catalog..."
                                          : "Wi-Fi: connecting...", false, 30'000);
        auto refreshed = net_.refresh_catalog([&](const platform::NetProgress& progress) {
          if (progress.total > 0) {
            const int pct = static_cast<int>(progress.received * 100 / progress.total);
            set_message("Catalog: " + std::to_string(pct) + "%");
          }
        });
        if (!refreshed) {
          set_message(refreshed.error().message, true);
        } else {
          catalog_offset_ = 0;
          library_dirty_ = true;
          set_message("Catalog updated");
        }
        break;
      }
      case NetCommand::Download: {
        set_message(net_.wifi_connected() ? ("Downloading " + job.entry.name + "...")
                                          : "Wi-Fi: connecting...", false, 30'000);
        auto downloaded = net_.download(job.entry, job.destination,
                                        [&](const platform::NetProgress& progress) {
          if (progress.total > 0) {
            const int pct = static_cast<int>(progress.received * 100 / progress.total);
            set_message("Download: " + std::to_string(pct) + "%");
          }
        });
        if (!downloaded) {
          set_message(downloaded.error().message, true);
        } else {
          set_message("Saved " + job.entry.name);
          library_dirty_ = true;
        }
        break;
      }
      case NetCommand::Hoard: {
        hoard_cancel_.store(false);
        {
          std::lock_guard lock(hoard_lock_);
          hoard_ = HoardState{};
          hoard_.active = true;
          hoard_.current = "refreshing catalog";
        }
        set_message("HOARD: refreshing catalog...", false, 0);
        auto refreshed = net_.refresh_catalog([&](const platform::NetProgress& progress) {
          std::lock_guard lock(hoard_lock_);
          if (progress.total > 0) {
            hoard_.percent = static_cast<int>(progress.received * 100 / progress.total);
          }
        });
        if (!refreshed) {
          {
            std::lock_guard lock(hoard_lock_);
            hoard_.active = false;
            hoard_.error = true;
          }
          set_message("HOARD failed: " + refreshed.error().message, true, 20'000);
          break;
        }
        auto total = library_.count(platform::LibraryFilter{});
        if (!total) {
          std::lock_guard lock(hoard_lock_);
          hoard_.active = false;
          hoard_.error = true;
          set_message("HOARD failed: cannot read catalog", true, 20'000);
          break;
        }
        {
          std::lock_guard lock(hoard_lock_);
          hoard_.total = *total;
          hoard_.current = "starting";
        }
        std::size_t done = 0;
        std::size_t failed = 0;
        std::size_t consecutive = 0;
        std::unordered_set<std::string> seen;
        auto walked = library_.for_each([&](const catalog::Entry& entry) {
          auto destination = library_.package_path(entry.id, entry.version);
          if (!destination) {
            ++done;
            ++failed;
            ++consecutive;
          } else if (!seen.insert(*destination).second) {
            ++done;
          } else {
            {
              std::lock_guard lock(hoard_lock_);
              hoard_.current = entry.name;
              hoard_.percent = 0;
            }
            auto current = net_.has_current(entry, *destination);
            if (current && *current) {
              ++done;
              {
                std::lock_guard lock(hoard_lock_);
                hoard_.done = done;
              }
              return !hoard_cancel_.load();
            }
            bool downloaded = false;
            std::string error;
            for (int attempt = 0; attempt < 2 && !downloaded; ++attempt) {
              auto result = net_.download(
                  entry, *destination, [&](const platform::NetProgress& progress) {
                    if (progress.total > 0) {
                      std::lock_guard lock(hoard_lock_);
                      hoard_.percent =
                          static_cast<int>(progress.received * 100 / progress.total);
                    }
                  });
              if (result) {
                downloaded = true;
              } else {
                error = result.error().message;
              }
            }
            ++done;
            if (!downloaded) {
              ++failed;
              ++consecutive;
            } else {
              consecutive = 0;
            }
            (void)error;
          }
          {
            std::lock_guard lock(hoard_lock_);
            hoard_.done = done;
            hoard_.failed = failed;
          }
          dirty_ = true;
          return !hoard_cancel_.load() && consecutive < 3;
        });
        {
          std::lock_guard lock(hoard_lock_);
          hoard_.active = false;
          hoard_.error = !walked;
          hoard_.done = done;
          hoard_.failed = failed;
        }
        library_dirty_ = true;
        if (!walked) {
          set_message("HOARD stopped: catalog read error", true, 20'000);
        } else if (hoard_cancel_.load()) {
          set_message("HOARD cancelled after " + std::to_string(done) + " entries", false,
                      20'000);
        } else if (failed > 0) {
          set_message("HOARD done: " + std::to_string(failed) + " failed", true, 20'000);
        } else {
          set_message("HOARD done: " + std::to_string(done) + " entries", false, 20'000);
        }
        break;
      }
    }
    dirty_ = true;
  }
}

void App::refresh_library(bool keep_shift) {
  std::size_t local_total = 0;
  state_.local = library_.local_packages(64, &local_total)
                     .value_or(std::vector<platform::LocalPackage>{});
  state_.local_total = local_total;
  if (library_filter_.device) {
    std::erase_if(state_.local, [&](const platform::LocalPackage& local) {
      return local.device != *library_filter_.device;
    });
  }
  if (library_filter_.variant != catalog::SmkVariant::All) {
    std::erase_if(state_.local, [&](const platform::LocalPackage& local) {
      return local.variant != library_filter_.variant;
    });
  }
  library_filter_.text = state_.filter;
  auto total = library_.count(library_filter_);
  catalog_total_ = total.value_or(0);
  const std::size_t page = 60;
  if (!keep_shift && catalog_offset_ > catalog_total_) catalog_offset_ = 0;
  auto entries = library_.page(library_filter_, catalog_offset_, page);
  state_.catalog = entries.value_or(std::vector<catalog::Entry>{});
  state_.catalog_total = catalog_total_;
  if (!library_filter_.device) {
    state_.device_filter = "ALL";
  } else if (*library_filter_.device == catalog::Device::Fm1) {
    state_.device_filter = "FM1";
  } else {
    state_.device_filter = "SMK";
  }
  if (library_filter_.device && *library_filter_.device == catalog::Device::Smk37) {
    switch (library_filter_.variant) {
      case catalog::SmkVariant::Pro: state_.chip_filter = "PRO"; break;
      case catalog::SmkVariant::Elite: state_.chip_filter = "ELITE"; break;
      case catalog::SmkVariant::Mke: state_.chip_filter = "MKE"; break;
      default: state_.chip_filter = "ALL"; break;
    }
  } else {
    switch (library_filter_.chip) {
      case platform::ChipFilter::New: state_.chip_filter = "NEW"; break;
      case platform::ChipFilter::Old: state_.chip_filter = "OLD"; break;
      default: state_.chip_filter = "ALL"; break;
    }
  }
  const int max_index = static_cast<int>(state_.local.size() + state_.catalog.size()) - 1;
  if (state_.selection > max_index) state_.selection = std::max(0, max_index);
  if (storage_.mounted()) {
    char line[160]{};
    std::snprintf(line, sizeof(line), "shown=%zu total=%zu free=%u largest=%u\n",
                  state_.local.size(), state_.local_total,
                  static_cast<unsigned>(
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    (void)storage_.write_text("fm1-card/logs/library.log", line);
  }
  dirty_ = true;
}

void App::sync_ui() {
  state_.midi = usb_.midi_ports();
  state_.msc = usb_.msc_ports();
  state_.routes.fill(0);
  for (std::size_t i = 0; i < state_.midi.size() && i < state_.routes.size(); ++i) {
    state_.routes[i] = router_.sinks(state_.midi[i].slot);
  }
  state_.clock_mask = router_.clock_targets();
  state_.bpm = clock_.bpm();
  state_.clock_running = clock_.running();
  state_.beat_phase = static_cast<uint32_t>(clock_.tick() % 24);
  state_.flash = flash_.status();
  state_.wifi = net_.wifi_connected();
  state_.wifi_state = net_.wifi_state();
  state_.sd_mounted = storage_.mounted();
  state_.battery = battery_.percent();
  {
    std::lock_guard lock(message_lock_);
    state_.message =
        (esp_timer_get_time() / 1000 < message_until_) ? message_ : std::string{};
    state_.message_error = message_is_error_;
  }
  {
    std::lock_guard lock(hoard_lock_);
    state_.hoard_active = hoard_.active;
    state_.hoard_error = hoard_.error;
    state_.hoard_done = hoard_.done;
    state_.hoard_total = hoard_.total;
    state_.hoard_failed = hoard_.failed;
    state_.hoard_current = hoard_.current;
    state_.hoard_percent = hoard_.percent;
  }
  state_.popup = popup_.kind != PopupKind::None;
  state_.popup_title = popup_.title;
  state_.popup_items = popup_.items;
  state_.popup_selection = popup_.selection;

  std::string json;
  json.reserve(1024);
  json += "{\"app\":\"fm1-card\",\"version\":\"";
  json += esp_app_get_description()->version;
  json += "\",\"sd\":";
  json += state_.sd_mounted ? "true" : "false";
  json += ",\"wifi\":\"";
  switch (state_.wifi_state) {
    case platform::WifiState::Connected: json += "connected"; break;
    case platform::WifiState::Connecting: json += "connecting"; break;
    case platform::WifiState::Failed: json += "failed"; break;
    default: json += "unconfigured"; break;
  }
  json += "\",\"devices\":[";
  char item[192]{};
  for (std::size_t i = 0; i < state_.midi.size(); ++i) {
    const auto& port = state_.midi[i];
    std::snprintf(item, sizeof(item),
                  "%s{\"slot\":%u,\"path\":\"%s\",\"identity\":\"%s\",\"clock\":%s}",
                  i == 0 ? "" : ",", static_cast<unsigned>(port.slot), port.path.c_str(),
                  port.identity.c_str(),
                  state_.clock_mask & (1u << port.slot) ? "true" : "false");
    json += item;
  }
  json += "],\"clock\":{\"bpm\":";
  std::snprintf(item, sizeof(item), "%lu,\"running\":%s}",
                static_cast<unsigned long>(state_.bpm),
                state_.clock_running ? "true" : "false");
  json += item;
  json += ",\"msc\":";
  std::snprintf(item, sizeof(item), "%zu", state_.msc.size());
  json += item;
  json += ",\"library\":{\"catalog\":";
  std::snprintf(item, sizeof(item), "%zu,\"local\":%zu,\"local_total\":%zu}",
                state_.catalog_total, state_.local.size(), state_.local_total);
  json += item;
  json += ",\"hoard\":";
  json += state_.hoard_active ? "true" : "false";
  json += "}";
  {
    std::lock_guard lock(status_lock_);
    status_json_ = std::move(json);
  }
}

void App::cycle_pane(int delta) {
  int pane = static_cast<int>(state_.pane) + delta;
  if (pane < 0) pane = 3;
  if (pane > 3) pane = 0;
  state_.pane = static_cast<ui::Pane>(pane);
  state_.selection = 0;
  state_.editing_filter = false;
  dirty_ = true;
}

void App::toggle_clock() {
  if (clock_.running()) {
    clock_.stop();
    for (const auto& port : usb_.midi_ports()) {
      if (router_.clock_enabled(port.slot) && !port.claimed) {
        const std::array<uint8_t, 1> stop{0xFC};
        usb_.send_sysex(port.slot, stop);
      }
    }
    set_message("Clock stopped");
  } else {
    if (!router_.any_clock()) {
      router_.set_clock_all(true);
      set_message("Clock targets: all");
    }
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    clock_.restart_phase(now);
    clock_.start(now);
    for (const auto& port : usb_.midi_ports()) {
      if (router_.clock_enabled(port.slot) && !port.claimed) {
        const std::array<uint8_t, 1> start{0xFA};
        usb_.send_sysex(port.slot, start);
      }
    }
    set_message("Clock running");
  }
  dirty_ = true;
}

void App::set_bpm(int delta) {
  const int bpm = std::clamp<int>(static_cast<int>(clock_.bpm()) + delta, clock::kMinBpm,
                                  clock::kMaxBpm);
  clock_.set_bpm(static_cast<uint32_t>(bpm));
  dirty_ = true;
}

void App::open_packages(int slot) {
  auto port = usb_.midi_port(slot);
  if (!port) return;
  popup_ = {};
  popup_.kind = PopupKind::Packages;
  popup_.title = "FLASH " + label_of(*port);
  popup_.context = slot;
  for (std::size_t i = 0; i < state_.local.size(); ++i) {
    const auto& local = state_.local[i];
    if (!port->identity.empty() &&
        !identity::package_fits(port->identity, local.product)) {
      continue;
    }
    popup_.items.push_back(local.product + "  " + std::to_string(local.size / 1024) + "K");
    popup_.values.push_back(static_cast<int>(i));
  }
  if (popup_.items.empty()) {
    popup_ = {};
    set_message("No compatible local package", true);
    return;
  }
  dirty_ = true;
}

void App::open_sinks(int source) {
  const auto& source_port = state_.midi[static_cast<std::size_t>(source)];
  popup_ = {};
  popup_.kind = PopupKind::Sinks;
  popup_.title = "ROUTE " + label_of(source_port);
  popup_.context = source_port.slot;
  for (const auto& port : state_.midi) {
    const bool on = router_.forwards(source_port.slot, port.slot);
    popup_.items.push_back(std::string(on ? "[x] " : "[ ] ") + label_of(port));
  }
  dirty_ = true;
}

void App::open_msc_actions(int slot) {
  popup_ = {};
  popup_.kind = PopupKind::MscActions;
  popup_.title = "ROM DISK " + std::to_string(slot);
  popup_.context = slot;
  popup_.items = {"Backup full flash", "Install rescue bootloader"};
  dirty_ = true;
}

void App::run_popup_action() {
  if (popup_.kind == PopupKind::Packages) {
    if (popup_.selection < 0 ||
        popup_.selection >= static_cast<int>(popup_.values.size())) {
      popup_ = {};
      return;
    }
    const auto& local = state_.local[static_cast<std::size_t>(
        popup_.values[static_cast<std::size_t>(popup_.selection)])];
    auto port = usb_.midi_port(static_cast<uint8_t>(popup_.context));
    const auto kind = (port && !port->identity.empty()) ? platform::FlashKind::Normal
                                                       : platform::FlashKind::Rescue;
    start_flash(kind, static_cast<uint8_t>(popup_.context), local.path);
    popup_ = {};
  } else if (popup_.kind == PopupKind::Sinks) {
    if (popup_.selection < 0 ||
        popup_.selection >= static_cast<int>(state_.midi.size())) {
      return;
    }
    const auto sink = state_.midi[static_cast<std::size_t>(popup_.selection)].slot;
    router_.toggle(static_cast<uint8_t>(popup_.context), sink);
    popup_.items[static_cast<std::size_t>(popup_.selection)] =
        std::string(router_.forwards(static_cast<uint8_t>(popup_.context), sink) ? "[x] "
                                                                               : "[ ] ") +
        label_of(state_.midi[static_cast<std::size_t>(popup_.selection)]);
    dirty_ = true;
  } else if (popup_.kind == PopupKind::HoardConfirm) {
    if (popup_.selection == 0) {
      queue_net({.command = NetCommand::Hoard, .entry = {}, .destination = {}});
      set_message("HOARD starting...", false, 10'000);
    }
    popup_ = {};
  } else if (popup_.kind == PopupKind::MscActions) {
    if (popup_.selection == 0) {
      start_flash(platform::FlashKind::Backup, static_cast<uint8_t>(popup_.context), {});
    } else {
      start_flash(platform::FlashKind::InstallBootloader,
                  static_cast<uint8_t>(popup_.context), {});
    }
    popup_ = {};
  }
}

void App::start_flash(platform::FlashKind kind, uint8_t slot, const std::string& package) {
  router_.set_clock(slot, false);
  platform::FlashJob job{.kind = kind,
                          .slot = slot,
                          .package_path = package,
                          .output_path = {}};
  if (!flash_.submit(job)) {
    set_message("A flash is already running", true);
    return;
  }
  set_message("Flashing...");
}

void App::handle_popup_key(const platform::KeyEvent& event) {
  if (event.kind != platform::KeyKind::Press) return;
  if (event.code == 0x29) {  // Esc
    popup_ = {};
    dirty_ = true;
    return;
  }
  if (event.code == 0x52 || event.code == 0x51) {  // Up/Down
    const int delta = event.code == 0x52 ? -1 : 1;
    popup_.selection = std::clamp(popup_.selection + delta, 0,
                                  std::max(0, static_cast<int>(popup_.items.size()) - 1));
    dirty_ = true;
    return;
  }
  if (event.code == 0x28) {  // Enter
    run_popup_action();
    dirty_ = true;
  }
}

void App::handle_key(const platform::KeyEvent& event) {
  {
    std::lock_guard lock(hoard_lock_);
    if (hoard_.active) {
      if (event.kind == platform::KeyKind::Press && event.code == 0x29) {
        hoard_cancel_.store(true);  // Esc
      }
      return;
    }
  }
  if (popup_.kind != PopupKind::None) {
    handle_popup_key(event);
    return;
  }
  if (event.kind != platform::KeyKind::Press) return;

  // Text filter takes every printable key.
  if (state_.editing_filter) {
    if (event.code == 0x28) {
      state_.editing_filter = false;
      catalog_offset_ = 0;
      library_dirty_ = true;
    } else if (event.code == 0x29) {
      state_.editing_filter = false;
      state_.filter.clear();
      catalog_offset_ = 0;
      library_dirty_ = true;
    } else if (event.code == 0x2A) {
      if (!state_.filter.empty()) state_.filter.pop_back();
      catalog_offset_ = 0;
      state_.selection = 0;
      library_dirty_ = true;
    } else if (event.character >= 32 && event.character < 127) {
      state_.filter.push_back(event.character);
      catalog_offset_ = 0;
      state_.selection = 0;
      library_dirty_ = true;
    }
    dirty_ = true;
    return;
  }

  switch (event.code) {
    case 0x3A: state_.pane = ui::Pane::Devices; state_.selection = 0; break;
    case 0x3B: state_.pane = ui::Pane::Routes; state_.selection = 0; break;
    case 0x3C: state_.pane = ui::Pane::Clock; state_.selection = 0; break;
    case 0x3D: state_.pane = ui::Pane::Library; state_.selection = 0; break;
    case 0x2B: cycle_pane(1); break;
    case 0x29:
      if (clock_.running()) {
        toggle_clock();
      } else {
        set_message("");
      }
      break;
    case 0x52:
    case 0x51: {
      const int delta = event.code == 0x52 ? -1 : 1;
      int limit = 0;
      switch (state_.pane) {
        case ui::Pane::Devices: limit = state_.midi.size() + state_.msc.size(); break;
        case ui::Pane::Routes:
        case ui::Pane::Clock: limit = state_.midi.size(); break;
        case ui::Pane::Library: limit = state_.local.size() + state_.catalog.size(); break;
      }
      constexpr std::size_t kLibraryPage = 5;
      if (state_.pane == ui::Pane::Library && event.ctrl) {
        if (delta > 0 && catalog_offset_ + kLibraryPage < catalog_total_) {
          catalog_offset_ += kLibraryPage;
          state_.selection = 0;
          library_dirty_ = true;
        } else if (delta < 0 && catalog_offset_ >= kLibraryPage) {
          catalog_offset_ -= kLibraryPage;
          state_.selection = 0;
          library_dirty_ = true;
        }
        break;
      }
      state_.selection = std::clamp(state_.selection + delta, 0, std::max(0, limit - 1));
      if (state_.pane == ui::Pane::Library && delta > 0 &&
          state_.selection + 1 >= static_cast<int>(state_.local.size() + state_.catalog.size()) &&
          catalog_offset_ + kLibraryPage < catalog_total_) {
        catalog_offset_ += kLibraryPage;
        state_.selection = 0;
        library_dirty_ = true;
      }
      break;
    }
    case 0x28: {  // Enter
      if (state_.pane == ui::Pane::Devices) {
        if (state_.selection < static_cast<int>(state_.midi.size())) {
          const auto& port = state_.midi[static_cast<std::size_t>(state_.selection)];
          router_.set_clock(port.slot, !router_.clock_enabled(port.slot));
        } else if (state_.selection >= static_cast<int>(state_.midi.size())) {
          open_msc_actions(state_.msc[static_cast<std::size_t>(
                                         state_.selection - state_.midi.size())]
                               .slot);
        }
      } else if (state_.pane == ui::Pane::Routes &&
                 state_.selection < static_cast<int>(state_.midi.size())) {
        open_sinks(state_.selection);
      } else if (state_.pane == ui::Pane::Clock &&
                 state_.selection < static_cast<int>(state_.midi.size())) {
        const auto slot = state_.midi[static_cast<std::size_t>(state_.selection)].slot;
        router_.set_clock(slot, !router_.clock_enabled(slot));
      } else if (state_.pane == ui::Pane::Library) {
        if (state_.selection >= static_cast<int>(state_.local.size())) {
          const auto index = static_cast<std::size_t>(state_.selection - state_.local.size());
          if (index < state_.catalog.size()) {
            auto path = library_.package_path(state_.catalog[index].id,
                                              state_.catalog[index].version);
            if (!path) {
              set_message(path.error().message, true);
            } else {
              queue_net({.command = NetCommand::Download,
                         .entry = state_.catalog[index],
                         .destination = *path});
              set_message("Download queued");
            }
          }
        }
      }
      break;
    }
    case 0x4C:  // Delete: clear routes
      if (state_.pane == ui::Pane::Routes) {
        router_.clear();
        set_message("Routes cleared");
      }
      break;
    default:
      break;
  }

  switch (event.character) {
    case ' ':
      if (state_.pane == ui::Pane::Clock) {
        toggle_clock();
      } else if (state_.pane == ui::Pane::Devices &&
                 state_.selection < static_cast<int>(state_.midi.size())) {
        const auto slot = state_.midi[static_cast<std::size_t>(state_.selection)].slot;
        router_.set_clock(slot, !router_.clock_enabled(slot));
      }
      break;
    case '+':
      if (state_.pane == ui::Pane::Clock) set_bpm(5);
      break;
    case '-':
      if (state_.pane == ui::Pane::Clock) set_bpm(-5);
      break;
    case 't':
    case 'T':
      if (state_.pane == ui::Pane::Clock) {
        router_.set_clock_all(!router_.any_clock());
      } else if (state_.pane == ui::Pane::Library) {
        if (!library_filter_.device) library_filter_.device = catalog::Device::Fm1;
        else if (*library_filter_.device == catalog::Device::Fm1)
          library_filter_.device = catalog::Device::Smk37;
        else library_filter_.device.reset();
        library_filter_.variant = catalog::SmkVariant::All;
        catalog_offset_ = 0;
        state_.selection = 0;
        library_dirty_ = true;
      }
      break;
    case 'c':
    case 'C':
      if (state_.pane == ui::Pane::Library) {
        if (library_filter_.device && *library_filter_.device == catalog::Device::Smk37) {
          library_filter_.variant =
              library_filter_.variant == catalog::SmkVariant::All
                  ? catalog::SmkVariant::Pro
                  : library_filter_.variant == catalog::SmkVariant::Pro
                        ? catalog::SmkVariant::Elite
                        : library_filter_.variant == catalog::SmkVariant::Elite
                              ? catalog::SmkVariant::Mke
                              : catalog::SmkVariant::All;
        } else {
          library_filter_.chip = library_filter_.chip == platform::ChipFilter::All
                                     ? platform::ChipFilter::New
                                     : library_filter_.chip == platform::ChipFilter::New
                                           ? platform::ChipFilter::Old
                                           : platform::ChipFilter::All;
        }
        catalog_offset_ = 0;
        state_.selection = 0;
        library_dirty_ = true;
      }
      break;
    case '/':
      if (state_.pane == ui::Pane::Library) state_.editing_filter = true;
      break;
    case 'r':
    case 'R':
      if (state_.pane == ui::Pane::Library) {
        queue_net({.command = NetCommand::Refresh, .entry = {}, .destination = {}});
        set_message(net_.wifi_connected() ? "Refreshing catalog..."
                                          : "Wi-Fi: connecting...", false, 30'000);
      }
      break;
    case 'd':
    case 'D':
      if (state_.pane == ui::Pane::Devices &&
          state_.selection < static_cast<int>(state_.midi.size())) {
        start_flash(platform::FlashKind::DownloadMode,
                    state_.midi[static_cast<std::size_t>(state_.selection)].slot, {});
      }
      break;
    case 'h':
    case 'H':
      if (state_.pane == ui::Pane::Library) {
        popup_ = {};
        popup_.kind = PopupKind::HoardConfirm;
        popup_.title = "HOARD ALL FIRMWARE?";
        popup_.items = {"YES - download all", "NO - cancel"};
        popup_.selection = 0;
        dirty_ = true;
      }
      break;
    case 'f':
    case 'F':
      if (state_.pane == ui::Pane::Devices &&
          state_.selection < static_cast<int>(state_.midi.size())) {
        open_packages(state_.midi[static_cast<std::size_t>(state_.selection)].slot);
      }
      break;
    case 'm':
    case 'M':
      if (state_.pane == ui::Pane::Devices &&
          state_.selection < static_cast<int>(state_.midi.size())) {
        start_flash(platform::FlashKind::DownloadMode,
                    state_.midi[static_cast<std::size_t>(state_.selection)].slot, {});
      }
      break;
    default:
      break;
  }
  dirty_ = true;
}

}  // namespace fm1

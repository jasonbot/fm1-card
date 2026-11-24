#include "app.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fm1/identity.hpp"

namespace fm1 {
namespace {

constexpr const char* kTag = "fm1-app";
constexpr std::size_t kPackagePage = 32;

void blit_cb(void* context, int x, int y, int width, int height,
             std::span<const uint16_t> pixels) {
  static_cast<platform::Display*>(context)->blit(x, y, width, height, pixels);
}

std::string label_of(const platform::MidiPort& port) {
  return port.identity.empty() ? ("MIDI " + port.path) : port.identity;
}

std::string lower_text(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Compare the numeric/lexical pieces of a firmware version. True when `have`
// is the same or newer than `wanted`.
bool version_at_least(std::string_view have, std::string_view wanted) {
  if (have.empty()) return false;
  if (wanted.empty()) return true;
  std::size_t a = 0;
  std::size_t b = 0;
  while (a < have.size() || b < wanted.size()) {
    while (a < have.size() && !std::isalnum(static_cast<unsigned char>(have[a]))) ++a;
    while (b < wanted.size() && !std::isalnum(static_cast<unsigned char>(wanted[b]))) ++b;
    const std::size_t ae = a;
    const std::size_t be = b;
    while (a < have.size() && std::isalnum(static_cast<unsigned char>(have[a]))) ++a;
    while (b < wanted.size() && std::isalnum(static_cast<unsigned char>(wanted[b]))) ++b;
    const auto ap = have.substr(ae, a - ae);
    const auto bp = wanted.substr(be, b - be);
    if (ap.empty()) return !bp.empty();
    if (bp.empty()) return true;
    const bool an = std::all_of(ap.begin(), ap.end(),
                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    const bool bn = std::all_of(bp.begin(), bp.end(),
                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (an && bn) {
      const auto av = std::strtoul(std::string(ap).c_str(), nullptr, 10);
      const auto bv = std::strtoul(std::string(bp).c_str(), nullptr, 10);
      if (av != bv) return av > bv;
    } else {
      const auto cmp = ap.compare(bp);
      if (cmp != 0) return cmp > 0;
    }
  }
  return true;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
        break;
    }
  }
  return out;
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
  if (storage_.mounted()) {
    if (auto config = platform::load_config(storage_); config) {
      device_name_ = config->device_name;
    }
  }
  queue_net({.command = NetCommand::Start, .entry = {}, .destination = {}, .ssid = {}, .password = {}, .name = {}});

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

void App::show_waiting(std::string_view text) {
  constexpr int kWidth = 180;
  constexpr int kHeight = 101;
  constexpr int kX = (240 - kWidth) / 2;
  constexpr int kY = (135 - kHeight) / 2;
  ui_.canvas().fill(kX, kY, kWidth, kHeight, 11);
  ui_.canvas().frame(kX, kY, kWidth, kHeight, 1);
  constexpr std::string_view kWaiting = "WAITING...";
  const int waiting_x = kX + (kWidth - static_cast<int>(kWaiting.size()) * 6) / 2;
  ui_.canvas().draw_text(waiting_x, kY + 8, kWaiting, 1, kX + kWidth);
  const int max_chars = (kWidth - 8) / 6;
  std::string detail(text.substr(0, std::min<std::size_t>(text.size(), max_chars)));
  const int detail_x = kX + (kWidth - static_cast<int>(detail.size()) * 6) / 2;
  const int detail_y = kY + (kHeight - 7) / 2;
  ui_.canvas().draw_text(detail_x, detail_y, detail, 1, kX + kWidth - 4);
  ui_.canvas().present(&display_, blit_cb);
  vTaskDelay(pdMS_TO_TICKS(1));
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

    if (net_.take_library_change()) library_dirty_ = true;
    if (library_dirty_.exchange(false)) {
      const bool filter_only = filter_refresh_;
      filter_refresh_ = false;
      refresh_library(false, !filter_only, true);
    }

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
          set_message("No Wi-Fi configured: open SYSTEM and press W", false, 15'000);
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
      case NetCommand::WifiScan: {
        set_message("Wi-Fi: scanning...", false, 30'000);
        auto scan = net_.scan_networks();
        {
          std::lock_guard lock(wifi_scan_lock_);
          wifi_scan_running_ = false;
          if (scan) {
            wifi_networks_ = std::move(*scan);
            wifi_scan_error_.clear();
          } else {
            wifi_networks_.clear();
            wifi_scan_error_ = scan.error().message;
          }
        }
        dirty_ = true;
        break;
      }
      case NetCommand::WifiConnect: {
        set_message("Wi-Fi: connecting to \"" + job.ssid + "\"...", false, 30'000);
        auto loaded = platform::load_config(storage_);
        platform::Config config = loaded ? *loaded : platform::Config{};
        config.ssid = job.ssid;
        config.password = job.password;
        if (storage_.mounted()) {
          auto saved = platform::save_config(storage_, config);
          if (!saved) {
            set_message("Wi-Fi config: " + saved.error().message, true, 15'000);
            break;
          }
        } else {
          set_message("Wi-Fi config not saved: no SD card", true, 15'000);
        }
        auto applied = net_.apply_wifi(config);
        if (!applied) {
          set_message("Wi-Fi: " + applied.error().message, true, 20'000);
        } else {
          library_dirty_ = true;
          set_message("Wi-Fi connected: " + job.ssid, false, 10'000);
        }
        break;
      }
      case NetCommand::SaveDeviceName: {
        auto loaded = platform::load_config(storage_);
        platform::Config config = loaded ? *loaded : platform::Config{};
        config.device_name = job.name;
        net_.set_device_name(job.name);
        if (!storage_.mounted()) {
          set_message("Device name active, not saved: no SD card", true, 15'000);
          break;
        }
        auto saved = platform::save_config(storage_, config);
        if (!saved) {
          set_message("Device name: " + saved.error().message, true, 15'000);
        } else {
          set_message("Device name: " + job.name, false, 8'000);
        }
        break;
      }
      case NetCommand::LoadLocalCatalog: {
        set_message("Catalog: reading local manifest...", false, 30'000);
        auto loaded = net_.load_local_catalog();
        if (!loaded) {
          set_message(loaded.error().message, true, 15'000);
        } else {
          catalog_offset_ = 0;
          library_dirty_ = true;
          set_message("Local manifest: " + std::to_string(*loaded) + " entries", false,
                      10'000);
        }
        break;
      }
    }
    dirty_ = true;
  }
}

void App::refresh_library(bool keep_shift, bool show_wait, bool reload_local) {
  if (show_wait) show_waiting("READING LIBRARY");
  if (reload_local) {
    std::size_t local_total = 0;
    state_.local = library_.local_packages_page(0, 32, &local_total, state_.filter)
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
  }
  library_filter_.text = state_.filter;
  auto total = library_.count(library_filter_);
  catalog_total_ = total.value_or(0);
  const std::size_t page = 20;
  if (!keep_shift && catalog_offset_ > catalog_total_) catalog_offset_ = 0;
  auto entries = library_.page(library_filter_, catalog_offset_, page);
  state_.catalog = entries.value_or(std::vector<catalog::Entry>{});
  state_.catalog_total = catalog_total_;
  const auto installed = library_.local_metadata().value_or(std::vector<platform::LocalMetadata>{});
  std::erase_if(state_.catalog, [&](const catalog::Entry& entry) {
    const auto entry_name = lower_text(entry.name);
    for (const auto& local : installed) {
      if (lower_text(local.name) != entry_name) continue;
      if (version_at_least(local.version, entry.version)) return true;
    }
    return false;
  });
  const int max_filtered = static_cast<int>(state_.local.size() + state_.catalog.size()) - 1;
  if (state_.selection > max_filtered) state_.selection = std::max(0, max_filtered);
  state_.catalog_installed.assign(state_.catalog.size(), false);
  for (std::size_t i = 0; i < state_.catalog.size(); ++i) {
    auto path = library_.package_path(state_.catalog[i].id, state_.catalog[i].version);
    struct stat info {};
    if (path && stat(path->c_str(), &info) == 0) {
      state_.catalog_installed[i] = true;
    }
  }
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
  state_.ssid = net_.wifi_ssid();
  state_.device_name = device_name_;
  state_.ip = net_.local_ip();
  state_.rssi = net_.wifi_rssi();
  state_.sd_mounted = storage_.mounted();
  const uint64_t battery_now = static_cast<uint64_t>(esp_timer_get_time());
  state_.battery = battery_.percent();
  if (last_battery_check_us_ == 0 || battery_now - last_battery_check_us_ > 15'000'000) {
    const int mv = battery_.millivolts();
    if (mv >= 0 && last_battery_mv_ >= 0) {
      if (mv >= last_battery_mv_ + 4) battery_charging_ = true;
      else if (mv + 4 < last_battery_mv_) battery_charging_ = false;
    }
    if (mv >= 0) last_battery_mv_ = mv;
    last_battery_check_us_ = battery_now;
  }
  state_.battery_charging = battery_charging_;
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
  if (popup_.kind == PopupKind::WifiNetworks) {
    std::lock_guard lock(wifi_scan_lock_);
    popup_.items.clear();
    if (wifi_scan_running_) {
      popup_.items.push_back("Scanning...");
    } else if (!wifi_scan_error_.empty()) {
      popup_.items.push_back(wifi_scan_error_);
    } else if (wifi_networks_.empty()) {
      popup_.items.push_back("No networks found");
      popup_.items.push_back("R rescan");
    } else {
      for (const auto& network : wifi_networks_) {
        char line[48]{};
        std::snprintf(line, sizeof(line), "%c %s  %d", network.secure ? '*' : ' ',
                      network.ssid.c_str(), network.rssi);
        popup_.items.emplace_back(line);
      }
    }
    popup_.selection = std::clamp(
        popup_.selection, 0, std::max(0, static_cast<int>(popup_.items.size()) - 1));
  } else if (popup_.kind == PopupKind::WifiPassword) {
    popup_.title = "WI-FI: " + wifi_ssid_;
    popup_.items.clear();
    popup_.items.push_back(std::string(wifi_password_.size(), '*') + "_");
    popup_.items.push_back("ENTER connect   ESC back");
    popup_.selection = 0;
  } else if (popup_.kind == PopupKind::DeviceName) {
    popup_.title = "DEVICE NAME";
    popup_.items.clear();
    popup_.items.push_back(name_edit_ + "_");
    popup_.items.push_back("ENTER save   ESC cancel");
    popup_.selection = 0;
  }
  state_.popup_hint =
      popup_.kind == PopupKind::Packages ? "ESC Back   ENTER Flash" : std::string{};
  state_.popup = popup_.kind != PopupKind::None;
  state_.popup_title = popup_.title;
  state_.popup_items = popup_.items;
  state_.popup_selection = popup_.selection;

  std::string json;
  json.reserve(1024);
  json += "{\"app\":\"fm1-card\",\"version\":\"";
  json += esp_app_get_description()->version;
  json += "\",\"name\":\"";
  json += json_escape(device_name_);
  json += "\",\"ip\":\"";
  {
    char ip[16]{};
    if (auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
      esp_netif_ip_info_t info{};
      if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
      }
    }
    json += ip;
  }
  json += "\",\"sd\":";
  json += state_.sd_mounted ? "true" : "false";
  json += ",\"wifi\":\"";
  switch (state_.wifi_state) {
    case platform::WifiState::Connected: json += "connected"; break;
    case platform::WifiState::Connecting: json += "connecting"; break;
    case platform::WifiState::Failed: json += "failed"; break;
    default: json += "unconfigured"; break;
  }
  json += "\",\"ssid\":\"" + state_.ssid + "\",\"rssi\":" +
          std::to_string(state_.rssi) + ",\"devices\":[";
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
  json += ",\"key\":\"" + json_escape(last_key_) + "\",\"flash\":{\"running\":";
  json += state_.flash.running ? "true" : "false";
  json += ",\"error\":";
  json += state_.flash.error ? "true" : "false";
  json += ",\"step\":\"" + json_escape(state_.flash.step) + "\",\"package\":\"" +
          json_escape(state_.flash.package) + "\"}";
  json += "}";
  {
    std::lock_guard lock(status_lock_);
    status_json_ = std::move(json);
  }
}

void App::cycle_pane(int delta) {
  int pane = static_cast<int>(state_.pane) + delta;
  if (pane < 0) pane = 4;
  if (pane > 4) pane = 0;
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
  if (!port) {
    set_message("That MIDI port is gone", true);
    return;
  }
  popup_ = {};
  popup_.kind = PopupKind::Packages;
  popup_.context = slot;
  load_package_page(0);
  if (package_total_ == 0) {
    popup_ = {};
    set_message("No local packages: download or upload one first", true);
    return;
  }
  dirty_ = true;
}

void App::load_package_page(std::size_t offset) {
  show_waiting("READING FIRMWARE LIST");
  std::size_t total = 0;
  auto page = library_.local_packages_page(offset, kPackagePage, &total);
  package_choices_ = page.value_or(std::vector<platform::LocalPackage>{});
  package_total_ = total;
  if (package_choices_.empty() && total > 0 && offset > 0) {
    const std::size_t last = ((total - 1) / kPackagePage) * kPackagePage;
    package_choices_ = library_.local_packages_page(last, kPackagePage, &total)
                           .value_or(std::vector<platform::LocalPackage>{});
    package_total_ = total;
    offset = last;
  }
  package_offset_ = offset;
  load_package_metadata();
  build_package_popup();
  popup_.selection = 0;
  dirty_ = true;
}

void App::load_package_metadata() {
  package_meta_.assign(package_choices_.size(), PackageMeta{});
  if (package_choices_.empty()) return;
  std::unordered_map<std::string, std::size_t> by_path;
  by_path.reserve(package_choices_.size());
  for (std::size_t i = 0; i < package_choices_.size(); ++i) {
    by_path.emplace(package_choices_[i].path, i);
  }
  (void)library_.for_each([&](const catalog::Entry& entry) {
    auto path = library_.package_path(entry.id, entry.version);
    if (!path) return true;
    const auto found = by_path.find(*path);
    if (found == by_path.end()) return true;
    PackageMeta& meta = package_meta_[found->second];
    meta.name = entry.name;
    if (!entry.version.empty()) meta.name += " " + entry.version;
    meta.new_chip = catalog::works_on_new_chip(entry);
    meta.old_chip = catalog::works_on_old_chip(entry);
    return true;
  });
}

void App::build_package_popup() {
  auto port = usb_.midi_port(static_cast<uint8_t>(popup_.context));
  if (!port) return;
  popup_.items.clear();
  popup_.values.clear();
  popup_.paths.clear();
  for (std::size_t choice = 0; choice < package_choices_.size(); ++choice) {
    const auto& local = package_choices_[choice];
    if (!port->identity.empty() &&
        !identity::package_fits(port->identity, local.product)) {
      continue;
    }
    if (library_filter_.device && local.device != *library_filter_.device) continue;
    if (local.device == catalog::Device::Smk37) {
      if (library_filter_.variant != catalog::SmkVariant::All &&
          local.variant != library_filter_.variant) {
        continue;
      }
    } else if (library_filter_.chip != platform::ChipFilter::All) {
      if (choice >= package_meta_.size()) continue;
      const auto& meta = package_meta_[choice];
      if (library_filter_.chip == platform::ChipFilter::New && !meta.new_chip) {
        continue;
      }
      if (library_filter_.chip == platform::ChipFilter::Old && !meta.old_chip) {
        continue;
      }
    }
    const bool has_meta = choice < package_meta_.size() && !package_meta_[choice].name.empty();
    std::string label = has_meta ? package_meta_[choice].name : local.name;
    if (label.empty()) label = local.product;
    if (has_meta) {
      if (package_meta_[choice].new_chip && package_meta_[choice].old_chip) label += " [ANY]";
      else if (package_meta_[choice].new_chip) label += " [NEW]";
      else if (package_meta_[choice].old_chip) label += " [OLD]";
      else label += " [?]";
    }
    popup_.items.push_back(std::move(label));
    popup_.values.push_back(static_cast<int>(choice));
  }
  std::string filter = "ALL";
  if (library_filter_.device) {
    filter = *library_filter_.device == catalog::Device::Smk37 ? "SMK" : "FM1";
    if (*library_filter_.device == catalog::Device::Smk37) {
      switch (library_filter_.variant) {
        case catalog::SmkVariant::Pro: filter += "/PRO"; break;
        case catalog::SmkVariant::Elite: filter += "/ELITE"; break;
        case catalog::SmkVariant::Mke: filter += "/MKE"; break;
        default: break;
      }
    } else {
      switch (library_filter_.chip) {
        case platform::ChipFilter::New: filter += "/NEW"; break;
        case platform::ChipFilter::Old: filter += "/OLD"; break;
        default: break;
      }
    }
  }
  const std::size_t page = package_offset_ / kPackagePage + 1;
  const std::size_t pages = std::max<std::size_t>(1, (package_total_ + kPackagePage - 1) / kPackagePage);
  popup_.title = "FLASH " + std::to_string(page) + "/" + std::to_string(pages) + " " + filter;
  if (popup_.items.empty()) {
    popup_.items.push_back("No matching packages");
  }
  popup_.selection = std::clamp(
      popup_.selection, 0, std::max(0, static_cast<int>(popup_.items.size()) - 1));
}

void App::open_flash_targets(int local_index) {
  if (local_index < 0 || local_index >= static_cast<int>(state_.local.size())) {
    set_message("Select an installed firmware first", true);
    return;
  }
  const auto& package = state_.local[static_cast<std::size_t>(local_index)];
  popup_ = {};
  popup_.kind = PopupKind::FlashTargets;
  popup_.title = "FLASH " + package.name;
  popup_.paths = {package.path};
  for (std::size_t i = 0; i < state_.midi.size(); ++i) {
    const auto& port = state_.midi[i];
    if (!port.identity.empty() && !identity::package_fits(port.identity, package.product)) {
      continue;
    }
    popup_.items.push_back(label_of(port));
    popup_.values.push_back(static_cast<int>(i));
  }
  if (popup_.items.empty()) {
    popup_ = {};
    set_message("No compatible live device", true);
    return;
  }
  popup_.selection = 0;
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

void App::open_wifi_dialog() {
  popup_ = {};
  popup_.kind = PopupKind::WifiNetworks;
  popup_.title = "WI-FI NETWORKS";
  popup_.selection = 0;
  {
    std::lock_guard lock(wifi_scan_lock_);
    wifi_networks_.clear();
    wifi_scan_error_.clear();
    wifi_scan_running_ = true;
  }
  queue_net({.command = NetCommand::WifiScan, .entry = {}, .destination = {}, .ssid = {}, .password = {}, .name = {}});
  dirty_ = true;
}

void App::open_name_dialog() {
  popup_ = {};
  popup_.kind = PopupKind::DeviceName;
  popup_.title = "DEVICE NAME";
  popup_.selection = 0;
  name_edit_ = device_name_;
  dirty_ = true;
}

void App::run_popup_action() {
  if (popup_.kind == PopupKind::Packages) {
    if (popup_.selection < 0 ||
        popup_.selection >= static_cast<int>(popup_.values.size())) {
      popup_ = {};
      return;
    }
    const int choice = popup_.values[static_cast<std::size_t>(popup_.selection)];
    if (choice < 0 || choice >= static_cast<int>(package_choices_.size())) {
      popup_ = {};
      return;
    }
    const std::string& path =
        package_choices_[static_cast<std::size_t>(choice)].path;
    auto port = usb_.midi_port(static_cast<uint8_t>(popup_.context));
    const auto kind = (port && !port->identity.empty()) ? platform::FlashKind::Normal
                                                       : platform::FlashKind::Rescue;
    start_flash(kind, static_cast<uint8_t>(popup_.context), path);
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
      queue_net({.command = NetCommand::Hoard, .entry = {}, .destination = {}, .ssid = {}, .password = {}, .name = {}});
      set_message("HOARD starting...", false, 10'000);
    }
    popup_ = {};
  } else if (popup_.kind == PopupKind::FlashTargets) {
    if (popup_.paths.empty() || popup_.selection < 0 ||
        popup_.selection >= static_cast<int>(popup_.values.size())) {
      popup_ = {};
      return;
    }
    const int target = popup_.values[static_cast<std::size_t>(popup_.selection)];
    if (target < 0 || target >= static_cast<int>(state_.midi.size())) {
      popup_ = {};
      return;
    }
    const auto& port = state_.midi[static_cast<std::size_t>(target)];
    state_.pane = ui::Pane::Devices;
    state_.selection = target;
    const auto kind = port.identity.empty() ? platform::FlashKind::Rescue
                                            : platform::FlashKind::Normal;
    start_flash(kind, port.slot, popup_.paths.front());
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
  show_waiting("STARTING FLASH");
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

void App::run_menu_action(int index) {
  switch (state_.pane) {
    case ui::Pane::Devices:
      if (state_.selection < static_cast<int>(state_.midi.size())) {
        const auto slot = state_.midi[static_cast<std::size_t>(state_.selection)].slot;
        if (index == 0) router_.set_clock(slot, !router_.clock_enabled(slot));
        else if (index == 1) open_packages(slot);
        else if (index == 2)
          start_flash(platform::FlashKind::DownloadMode, slot, {});
        else if (index > 2) set_message("Only MIDI devices can flash", true);
      } else if (index == 0) {
        const auto msc = static_cast<std::size_t>(state_.selection - state_.midi.size());
        if (msc < state_.msc.size()) open_msc_actions(state_.msc[msc].slot);
      } else {
        set_message("Select a MIDI device first", true);
      }
      break;
    case ui::Pane::Routes:
      if (index == 0 && state_.selection < static_cast<int>(state_.midi.size())) {
        open_sinks(state_.selection);
      } else if (index == 1) {
        router_.clear();
        set_message("Routes cleared");
      }
      break;
    case ui::Pane::Clock:
      if (index == 0) {
        toggle_clock();
      } else if (index == 1) {
        router_.set_clock_all(!router_.any_clock());
      } else if (index == 2) {
        set_bpm(-5);
      } else if (index == 3) {
        set_bpm(5);
      } else if (index == 4 && state_.selection < static_cast<int>(state_.midi.size())) {
        const auto slot = state_.midi[static_cast<std::size_t>(state_.selection)].slot;
        router_.set_clock(slot, !router_.clock_enabled(slot));
      }
      break;
    case ui::Pane::Library:
      if (index == 0) {
        queue_net({.command = NetCommand::Refresh,
                   .entry = {},
                   .destination = {},
                   .ssid = {},
                   .password = {},
                   .name = {}});
        set_message(net_.wifi_connected() ? "Refreshing catalog..."
                                          : "Wi-Fi: connecting...", false, 30'000);
      } else if (index == 1) {
        queue_net({.command = NetCommand::LoadLocalCatalog,
                   .entry = {},
                   .destination = {},
                   .ssid = {},
                   .password = {},
                   .name = {}});
        set_message("Loading local catalog manifest...", false, 10'000);
      } else if (index == 2) {
        if (!library_filter_.device) library_filter_.device = catalog::Device::Fm1;
        else if (*library_filter_.device == catalog::Device::Fm1)
          library_filter_.device = catalog::Device::Smk37;
        else library_filter_.device.reset();
        library_filter_.variant = catalog::SmkVariant::All;
        catalog_offset_ = 0;
        state_.selection = 0;
        library_dirty_ = true;
      } else if (index == 3) {
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
      } else if (index == 4) {
        popup_ = {};
        popup_.kind = PopupKind::HoardConfirm;
        popup_.title = "HOARD ALL FIRMWARE?";
        popup_.items = {"YES - download all", "NO - cancel"};
        popup_.selection = 0;
        dirty_ = true;
      } else if (index == 5) {
        state_.editing_filter = true;
      } else if (index == 6) {
        open_flash_targets(state_.selection);
      }
      break;
    case ui::Pane::System:
      if (index == 0) open_wifi_dialog();
      else if (index == 1) open_name_dialog();
      break;
  }
  dirty_ = true;
}

void App::handle_popup_key(const platform::KeyEvent& event) {
  if (event.kind != platform::KeyKind::Press) return;
  if (popup_.kind == PopupKind::WifiNetworks) {
    if (event.code == 0x29) {
      popup_ = {};
      dirty_ = true;
      return;
    }
    if (event.code == 0x52 || event.code == 0x51) {
      const int delta = event.code == 0x52 ? -1 : 1;
      popup_.selection = std::clamp(popup_.selection + delta, 0,
                                    std::max(0, static_cast<int>(popup_.items.size()) - 1));
      dirty_ = true;
      return;
    }
    if (event.character == 'r' || event.character == 'R') {
      open_wifi_dialog();
      return;
    }
    if (event.code == 0x28) {
      std::lock_guard lock(wifi_scan_lock_);
      if (!wifi_scan_running_ && wifi_scan_error_.empty() &&
          popup_.selection < static_cast<int>(wifi_networks_.size())) {
        wifi_ssid_ = wifi_networks_[static_cast<std::size_t>(popup_.selection)].ssid;
        wifi_password_.clear();
        popup_.kind = PopupKind::WifiPassword;
        popup_.selection = 0;
        dirty_ = true;
      }
    }
    return;
  }
  if (popup_.kind == PopupKind::WifiPassword) {
    if (event.code == 0x29) {
      popup_.kind = PopupKind::WifiNetworks;
      popup_.selection = std::clamp(popup_.selection, 0,
                                    std::max(0, static_cast<int>(popup_.items.size()) - 1));
      dirty_ = true;
      return;
    }
    if (event.code == 0x2A) {
      if (!wifi_password_.empty()) wifi_password_.pop_back();
      dirty_ = true;
      return;
    }
    if (event.code == 0x28) {
      queue_net({.command = NetCommand::WifiConnect,
                 .entry = {},
                 .destination = {},
                 .ssid = wifi_ssid_,
                 .password = wifi_password_,
                 .name = {}});
      popup_ = {};
      wifi_password_.clear();
      dirty_ = true;
      return;
    }
    if (event.character >= 32 && event.character < 127 &&
        wifi_password_.size() < 63) {
      wifi_password_.push_back(event.character);
      dirty_ = true;
    }
    return;
  }
  if (popup_.kind == PopupKind::Packages) {
    if (event.character == 't' || event.character == 'T') {
      if (!library_filter_.device) {
        library_filter_.device = catalog::Device::Fm1;
      } else if (*library_filter_.device == catalog::Device::Fm1) {
        library_filter_.device = catalog::Device::Smk37;
      } else {
        library_filter_.device.reset();
      }
      library_filter_.variant = catalog::SmkVariant::All;
      load_package_page(0);
      return;
    }
    if (event.character == 'c' || event.character == 'C') {
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
      load_package_page(0);
      return;
    }
    if (event.ctrl && (event.code == 0x52 || event.code == 0x51)) {
      constexpr int kSkip = 5;
      const int delta = event.code == 0x52 ? -kSkip : kSkip;
      const int target = popup_.selection + delta;
      if (target >= 0 && target < static_cast<int>(popup_.items.size())) {
        popup_.selection = target;
        dirty_ = true;
        return;
      }
      if (target >= static_cast<int>(popup_.items.size()) &&
          package_offset_ + kPackagePage < package_total_) {
        const int overflow = target - static_cast<int>(popup_.items.size());
        load_package_page(package_offset_ + kPackagePage);
        popup_.selection = std::clamp(overflow, 0,
                                      std::max(0, static_cast<int>(popup_.items.size()) - 1));
        return;
      }
      if (target < 0 && package_offset_ > 0) {
        load_package_page(package_offset_ - kPackagePage);
        popup_.selection = std::clamp(static_cast<int>(popup_.items.size()) + target, 0,
                                      std::max(0, static_cast<int>(popup_.items.size()) - 1));
        return;
      }
      popup_.selection = std::clamp(target, 0,
                                    std::max(0, static_cast<int>(popup_.items.size()) - 1));
      dirty_ = true;
      return;
    }
    if (event.code == 0x51 &&
        popup_.selection + 1 >= static_cast<int>(popup_.items.size()) &&
        package_offset_ + kPackagePage < package_total_) {
      load_package_page(package_offset_ + kPackagePage);
      return;
    }
    if (event.code == 0x52 && popup_.selection == 0 && package_offset_ > 0) {
      const std::size_t page = package_offset_ / kPackagePage;
      load_package_page((page - 1) * kPackagePage);
      popup_.selection = std::max(0, static_cast<int>(popup_.items.size()) - 1);
      return;
    }
  }
  if (popup_.kind == PopupKind::DeviceName) {
    if (event.code == 0x29) {
      popup_ = {};
      dirty_ = true;
      return;
    }
    if (event.code == 0x2A) {
      if (!name_edit_.empty()) name_edit_.pop_back();
      dirty_ = true;
      return;
    }
    if (event.code == 0x28) {
      if (name_edit_.empty()) {
        set_message("Name cannot be empty", true);
        return;
      }
      name_edit_ = platform::normalize_device_name(name_edit_);
      device_name_ = name_edit_;
      queue_net({.command = NetCommand::SaveDeviceName,
                 .entry = {},
                 .destination = {},
                 .ssid = {},
                 .password = {},
                 .name = name_edit_});
      popup_ = {};
      dirty_ = true;
      return;
    }
    if (event.character >= 32 && event.character < 127 && name_edit_.size() < 32) {
      name_edit_.push_back(event.character);
      dirty_ = true;
    }
    return;
  }
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
  if (event.kind == platform::KeyKind::Press) {
    char key[48]{};
    std::snprintf(key, sizeof(key), "c=%d code=%d fn=%d sh=%d", event.character,
                  static_cast<int>(event.code), event.fn ? 1 : 0, event.shift ? 1 : 0);
    last_key_ = key;
  }
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
      filter_refresh_ = true;
      catalog_offset_ = 0;
      state_.selection = 0;
      library_dirty_ = true;
    } else if (event.code == 0x29) {
      state_.editing_filter = false;
      filter_refresh_ = true;
      state_.filter.clear();
      catalog_offset_ = 0;
      state_.selection = 0;
      library_dirty_ = true;
    } else if (event.code == 0x2A) {
      if (!state_.filter.empty()) state_.filter.pop_back();
    } else if (event.character >= 32 && event.character < 127) {
      state_.filter.push_back(event.character);
    }
    dirty_ = true;
    return;
  }

  if (event.character >= '1' && event.character <= '9') {
    run_menu_action(event.character - '1');
    return;
  }
  if (const auto pos = std::string_view{"!@#$%^&*("}.find(event.character);
      pos != std::string_view::npos) {
    run_menu_action(static_cast<int>(pos));
    return;
  }
  if (event.code >= 0x3A && event.code <= 0x42) {
    run_menu_action(event.code - 0x3A);
    return;
  }

  switch (event.code) {
    case 0x2B: cycle_pane(event.ctrl ? -1 : 1); break;
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
        case ui::Pane::System: limit = 0; break;
      }
      constexpr std::size_t kLibraryPage = 20;
      constexpr int kLibrarySkip = 5;
      if (state_.pane == ui::Pane::Library && event.ctrl) {
        const int target = state_.selection + delta * kLibrarySkip;
        if (target < 0) {
          if (catalog_offset_ >= kLibraryPage) {
            catalog_offset_ -= kLibraryPage;
            state_.selection = static_cast<int>(state_.local.size());
            library_dirty_ = true;
          } else {
            state_.selection = 0;
            dirty_ = true;
          }
        } else if (target >= limit) {
          if (catalog_offset_ + kLibraryPage < catalog_total_) {
            catalog_offset_ += kLibraryPage;
            state_.selection = static_cast<int>(state_.local.size());
            library_dirty_ = true;
          } else {
            state_.selection = std::max(0, limit - 1);
            dirty_ = true;
          }
        } else {
          state_.selection = target;
          dirty_ = true;
        }
        break;
      }
      state_.selection = std::clamp(state_.selection + delta, 0, std::max(0, limit - 1));
      if (state_.pane == ui::Pane::Library && delta > 0 &&
          state_.selection + 1 >= static_cast<int>(state_.local.size() + state_.catalog.size()) &&
          catalog_offset_ + kLibraryPage < catalog_total_) {
        catalog_offset_ += kLibraryPage;
        state_.selection = static_cast<int>(state_.local.size());
        library_dirty_ = true;
      }
      break;
    }
    case 0x28: {  // Enter
      if (state_.pane == ui::Pane::Devices) {
        if (state_.selection < static_cast<int>(state_.midi.size())) {
          open_packages(state_.midi[static_cast<std::size_t>(state_.selection)].slot);
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
                         .destination = *path,
                         .ssid = {},
                         .password = {},
                         .name = {}});
              set_message("Download queued");
            }
          }
        }
      }
      break;
    }
    default:
      break;
  }


  dirty_ = true;
}

}  // namespace fm1

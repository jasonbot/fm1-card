#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fm1/catalog.hpp"
#include "fm1/platform/board.hpp"
#include "fm1/platform/config.hpp"
#include "fm1/result.hpp"

#include "esp_http_server.h"

namespace fm1::platform {

struct NetProgress {
  std::size_t received = 0;
  std::size_t total = 0;
  std::string_view phase;
};

enum class WifiState : uint8_t { Unconfigured, Connecting, Connected, Failed };

struct WifiNetwork {
  std::string ssid;
  int rssi = -127;
  bool secure = true;
};

class Net {
 public:
  using ProgressFn = std::function<void(const NetProgress&)>;

  Result<void> start(Storage& storage, const Config& config);
  void set_status_provider(std::function<std::string()> provider);
  // Called by the HTTP server when a remote upload lands.
  void note_library_change() { library_changed_.store(true); }
  [[nodiscard]] bool take_library_change() {
    return library_changed_.exchange(false);
  }
  esp_err_t list_firmwares(httpd_req_t* request);
  esp_err_t upload_firmware(httpd_req_t* request);
  esp_err_t firmware_info(httpd_req_t* request);
  [[nodiscard]] std::string status_json() const;
  [[nodiscard]] bool wifi_connected() const;
  [[nodiscard]] bool wifi_configured() const { return configured_.load(); }
  [[nodiscard]] bool wifi_reconfiguring() const { return reconfiguring_.load(); }
  [[nodiscard]] WifiState wifi_state() const { return state_.load(); }
  [[nodiscard]] std::string wifi_error() const;
  [[nodiscard]] std::string wifi_ssid() const;
  [[nodiscard]] std::string local_ip() const;
  [[nodiscard]] int wifi_rssi() const;
  // Scans visible 2.4 GHz networks. Blocks for a few seconds; call from the
  // network task, never the UI task.
  Result<std::vector<WifiNetwork>> scan_networks();
  // Applies new credentials and reconnects without a reboot.
  Result<void> apply_wifi(const Config& config);
  void set_device_name(std::string name);
  void note_connected();
  void note_disconnected();
  void stop();

  Result<void> refresh_catalog(const ProgressFn& progress);
  Result<void> download(const catalog::Entry& entry, const std::string& destination,
                        const ProgressFn& progress);
  // True when destination and its .meta sidecar match this entry and the
  // local file still hashes to the recorded digest.
  Result<bool> has_current(const catalog::Entry& entry, const std::string& destination);

 private:
  Result<void> fetch(const std::string& url, const ProgressFn& progress,
                     const std::function<bool(std::span<const char>)>& sink);
  Result<void> ensure_wifi_driver();
  Result<void> connect_wifi();
  Result<void> ensure_connected();
  Result<void> start_http();
  void start_discovery();
  void stop_discovery();
  static void discovery_entry(void* arg);
  void discovery_loop();

 public:
  [[nodiscard]] void* event_group() const { return event_group_; }

 private:

  Storage* storage_ = nullptr;
  std::string catalog_url_;
  std::string ssid_;
  mutable std::mutex name_lock_;
  std::string device_name_ = "Cardputer";
  mutable std::atomic<WifiState> state_{WifiState::Unconfigured};
  mutable std::mutex error_lock_;
  std::string wifi_error_;
  void* event_group_ = nullptr;
  void* http_ = nullptr;
  int discovery_socket_ = -1;
  std::atomic<bool> discovery_running_{false};
  std::function<std::string()> status_provider_;
  std::atomic<bool> configured_{false};
  std::atomic<bool> library_changed_{false};
  bool started_ = false;
  bool driver_ready_ = false;
  std::atomic<bool> reconfiguring_{false};
};

}  // namespace fm1::platform

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

#include "fm1/catalog.hpp"
#include "fm1/clock.hpp"
#include "fm1/platform/board.hpp"
#include "fm1/platform/flash.hpp"
#include "fm1/platform/library.hpp"
#include "fm1/platform/net.hpp"
#include "fm1/platform/usb.hpp"
#include "fm1/routing.hpp"
#include "fm1/ui/ui.hpp"

namespace fm1 {

class App {
 public:
  App(platform::Display& display, platform::Keyboard& keyboard, platform::Storage& storage,
      platform::Battery& battery);
  void run();
  void ui_loop();
  void clock_loop();
  void net_loop();

 private:
  enum class NetCommand { Start, Download, Hoard, WifiScan, WifiConnect, SaveDeviceName };
  struct NetJob {
    NetCommand command = NetCommand::Start;
    catalog::Entry entry;
    std::string destination;
    std::string ssid;
    std::string password;
    std::string name;
  };

  enum class PopupKind {
    None,
    Packages,
    Sinks,
    MscActions,
    HoardConfirm,
    WifiNetworks,
    WifiPassword,
    DeviceName,
    FlashTargets
  };
  struct Popup {
    PopupKind kind = PopupKind::None;
    std::string title;
    std::vector<std::string> items;
    std::vector<int> values;
    std::vector<std::string> paths;
    int selection = 0;
    int context = 0;
  };

  void handle_key(const platform::KeyEvent& event);
  void handle_popup_key(const platform::KeyEvent& event);
  void open_wifi_dialog();
  void open_name_dialog();
  void sync_ui();
  void refresh_library(bool keep_shift = false, bool show_waiting = true, bool reload_local = true);
  void set_message(std::string message, bool error = false, uint32_t ttl_ms = 6000);
  void show_waiting(std::string_view text);
  void run_menu_action(int index);
  void boot_stage(std::string_view stage, bool error = false);
  void cycle_pane(int delta);
  void toggle_clock();
  void set_bpm(int delta);
  void open_packages(int slot);
  void open_flash_targets(int local_index);
  void load_package_page(std::size_t offset);
  void build_package_popup();
  void load_package_metadata();
  void open_sinks(int source);
  void open_msc_actions(int slot);
  void run_popup_action();
  void start_flash(platform::FlashKind kind, uint8_t slot, const std::string& package);
  void queue_net(NetJob job);

  platform::Display& display_;
  platform::Keyboard& keyboard_;
  platform::Storage& storage_;
  platform::Battery& battery_;
  platform::Audio audio_;
  platform::UsbHost& usb_ = platform::UsbHost::instance();
  platform::FlashWorker flash_;
  platform::Net net_;
  platform::Library library_;
  routing::Router router_;
  clock::Clock clock_;

  ui::Ui ui_;
  ui::UiState state_;
  Popup popup_;

  std::mutex message_lock_;
  std::mutex status_lock_;
  std::string status_json_;
  std::mutex hoard_lock_;
  std::atomic<bool> hoard_cancel_{false};
  struct HoardState {
    bool active = false;
    std::size_t done = 0;
    std::size_t total = 0;
    std::size_t failed = 0;
    std::string current;
    int percent = 0;
    bool error = false;
  } hoard_;
  std::mutex net_lock_;
  std::condition_variable net_wake_;
  std::deque<NetJob> net_jobs_;

  std::atomic<bool> dirty_{true};
  std::atomic<bool> library_dirty_{true};
  std::string message_;
  bool message_is_error_ = false;
  uint64_t message_until_ = 0;
  std::size_t catalog_offset_ = 0;
  std::size_t catalog_total_ = 0;
  platform::LibraryFilter library_filter_;
  struct PackageMeta {
    std::string name;
    bool new_chip = false;
    bool old_chip = false;
  };
  std::vector<platform::LocalPackage> package_choices_;
  std::vector<PackageMeta> package_meta_;
  std::size_t package_offset_ = 0;
  std::size_t local_offset_ = 0;
  std::vector<platform::LocalMetadata> installed_metadata_;
  bool installed_metadata_valid_ = false;
  std::size_t package_total_ = 0;

  std::mutex wifi_scan_lock_;
  std::vector<platform::WifiNetwork> wifi_networks_;
  bool wifi_scan_running_ = false;
  std::string wifi_scan_error_;
  std::string wifi_ssid_;
  std::string wifi_password_;
  std::string device_name_ = "Cardputer";
  std::string name_edit_;
  int last_battery_mv_ = -1;
  uint64_t last_battery_check_us_ = 0;
  bool battery_charging_ = false;
  uint64_t charge_seen_us_ = 0;
  bool filter_refresh_ = false;
};

}  // namespace fm1

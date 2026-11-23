#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
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
  enum class NetCommand { Start, Refresh, Download };
  struct NetJob {
    NetCommand command = NetCommand::Start;
    catalog::Entry entry;
    std::string destination;
  };

  enum class PopupKind { None, Packages, Sinks, MscActions };
  struct Popup {
    PopupKind kind = PopupKind::None;
    std::string title;
    std::vector<std::string> items;
    std::vector<int> values;
    int selection = 0;
    int context = 0;
  };

  void handle_key(const platform::KeyEvent& event);
  void handle_popup_key(const platform::KeyEvent& event);
  void sync_ui();
  void refresh_library(bool keep_shift = false);
  void set_message(std::string message, bool error = false);
  void cycle_pane(int delta);
  void toggle_clock();
  void set_bpm(int delta);
  void open_packages(int slot);
  void open_sinks(int source);
  void open_msc_actions(int slot);
  void run_popup_action();
  void start_flash(platform::FlashKind kind, uint8_t slot, const std::string& package);
  void queue_net(NetJob job);

  platform::Display& display_;
  platform::Keyboard& keyboard_;
  platform::Storage& storage_;
  platform::Battery& battery_;
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

};

}  // namespace fm1

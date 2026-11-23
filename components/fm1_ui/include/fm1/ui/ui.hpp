#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fm1/catalog.hpp"
#include "fm1/platform/flash.hpp"
#include "fm1/platform/library.hpp"
#include "fm1/platform/net.hpp"
#include "fm1/platform/usb.hpp"
#include "fm1/ui/canvas.hpp"

namespace fm1::ui {

enum class Pane : uint8_t { Devices, Routes, Clock, Library };

struct UiState {
  Pane pane = Pane::Devices;
  int selection = 0;
  std::string filter;
  bool editing_filter = false;

  std::vector<platform::MidiPort> midi;
  std::vector<platform::MscPort> msc;
  std::array<uint8_t, 8> routes{};
  uint8_t clock_mask = 0;
  uint32_t beat_phase = 0;
  std::vector<catalog::Entry> catalog;
  std::vector<platform::LocalPackage> local;
  std::size_t catalog_total = 0;
  std::size_t local_total = 0;
  std::string device_filter = "ALL";
  std::string chip_filter = "ALL";

  platform::FlashStatus flash;
  uint32_t bpm = 120;
  bool clock_running = false;
  std::string clock_targets;
  bool wifi = false;
  bool sd_mounted = false;
  platform::WifiState wifi_state = platform::WifiState::Unconfigured;
  int battery = -1;
  std::string message;
  bool message_error = false;
  std::string detail;
  bool popup = false;
  bool hoard_active = false;
  bool hoard_error = false;
  std::size_t hoard_done = 0;
  std::size_t hoard_total = 0;
  std::size_t hoard_failed = 0;
  std::string hoard_current;
  int hoard_percent = 0;
  std::string popup_title;
  std::vector<std::string> popup_items;
  int popup_selection = 0;
};

class Ui {
 public:
  void draw(const UiState& state);
  Canvas& canvas() { return canvas_; }

 private:
  void header(const UiState& state);
  void button(int x, int y, int w, int h, std::string_view label, bool active);
  void tabs(const UiState& state);
  void devices(const UiState& state, int first_row, int rows);
  void routes(const UiState& state, int first_row, int rows);
  void clock(const UiState& state, int first_row, int rows);
  void library(const UiState& state, int first_row, int rows);
  void flash(const UiState& state);
  void popup(const UiState& state);
  void hoard(const UiState& state);
  void scrollbar(int first, int visible, int total, int y, int height);
  void footer(const UiState& state);

  Canvas canvas_;
};

}  // namespace fm1::ui

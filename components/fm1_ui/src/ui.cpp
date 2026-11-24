#include "fm1/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace fm1::ui {
namespace {

std::string trim(std::string_view text, std::size_t width) {
  if (text.size() <= width) return std::string(text);
  if (width <= 2) return std::string(width, ' ');
  return std::string(text.substr(0, width - 2)) + "..";
}

char checkbox(bool on) {
  return on ? 'x' : ' ';
}

}  // namespace

namespace {

constexpr const char* kDevTab[] = {
    "             ", "             ", " ########### ", " # # # # # # ",
    " # # # # # # ", " # # # # # # ", " #         # ", " #         # ",
    "  #########  ", "             ", "             ",
};
constexpr const char* kRouteTab[] = {
    "            ", "            ", " #  ####### ", "  # # # # # ", "   ## # # # ", "  ###     # ",
    "   ##     # ", "  # #     # ", " #  ####### ", "            ", "            ",
};
constexpr const char* kClockTab[] = {
    "         ", "    #    ", "   # #   ", "  # # #  ", "  # # #  ", " #  #  # ",
    " #  #  # ", " #     # ", "  #####  ", "         ", "         ",
};
constexpr const char* kLibraryTab[] = {
    "           ", "           ", " # #   # # ", " # #   # # ", " # #  #  # ", " # #  #  # ",
    " # #  #  # ", " # # #   # ", " # # #   # ", "           ", "           ",
};
constexpr const char* kSystemTab[] = {
    "             ", "             ", "  #########  ", " #         # ",
    " # ####### # ", " #         # ", " # # # # # # ", "  #       #  ",
    " ########### ", "             ", "             ",
};
constexpr const char* kWifiIcon[] = {
    "BWWWWWWWWWB", "WBBBBBBBBBW", "WBGGGGGGGBW", "WBGGGGGGGBW", "BWBGGGGGBWB",
    "BBWBGGGBWBB", "BBBWBGBWBBB", "BBBBWBWBBBB", "BBBBBWBBBBB", "BBBBBBBBBBB",
};
constexpr const char* kSdIcon[] = {
    "WWWWWWWWW", "WGBGBGBGW", "WBGBGBGBW", "WGBGBGBGW", "WGGGGGGGW",
    "WGWGWGWGW", "WGWGWGWGW", "WGWGWGWGW", "WWWWWWWWB", "BBBBBBBBB",
};
constexpr const char* kBatteryIcon[] = {
    "BWWWWWWWWBB", "WBBBBBBBBWB", "WBGGGGGGBWB", "WBGGGGGGBWW", "WBGGGGGGBWW",
    "WBGGGGGGBWW", "WBGGGGGGBWB", "WBBBBBBBBWB", "BWWWWWWWWBB", "BBBBBBBBBBB",
};

template <std::size_t H>
void draw_tab_glyph(Canvas& canvas, int x, int y, const char* const (&rows)[H], int w,
                    bool selected) {
  if (selected) {
    for (int row = 0; row < static_cast<int>(H); ++row) {
      for (int col = 0; col < w; ++col) {
        if (rows[row][col] != '#') continue;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            canvas.pixel(x + col + dx, y + row + dy, 11);
          }
        }
      }
    }
  }
  for (int row = 0; row < static_cast<int>(H); ++row) {
    for (int col = 0; col < w; ++col) {
      if (rows[row][col] == '#') canvas.pixel(x + col, y + row, 1);
    }
  }
}

template <std::size_t H>
void draw_wifi_icon(Canvas& canvas, int x, int y, const char* const (&rows)[H], int w,
                    bool connected, int rssi) {
  int min_y = static_cast<int>(H);
  int max_y = -1;
  for (int row = 0; row < static_cast<int>(H); ++row) {
    for (int col = 0; col < w; ++col) {
      if (rows[row][col] == 'G') {
        min_y = std::min(min_y, row);
        max_y = std::max(max_y, row);
      }
    }
  }
  const int level = connected ? std::clamp((rssi + 90) * 100 / 50, 0, 100) : 0;
  const int span = std::max(1, max_y - min_y);
  const int lit_from = level == 0 ? static_cast<int>(H) : max_y - span * level / 100;
  for (int row = 0; row < static_cast<int>(H); ++row) {
    for (int col = 0; col < w; ++col) {
      const char c = rows[row][col];
      if (c == '.') continue;
      const uint8_t color = c == 'W' ? 11 : c == 'B' ? 3 : (row >= lit_from ? 11 : 3);
      canvas.pixel(x + col, y + row, color);
    }
  }
}

template <std::size_t H>
void draw_sd_icon(Canvas& canvas, int x, int y, const char* const (&rows)[H], int w, bool mounted) {
  for (int row = 0; row < static_cast<int>(H); ++row) {
    for (int col = 0; col < w; ++col) {
      const char c = rows[row][col];
      if (c == '.') continue;
      const uint8_t color = c == 'W' ? 11 : c == 'B' ? 3 : (mounted ? 13 : 3);
      canvas.pixel(x + col, y + row, color);
    }
  }
}

template <std::size_t H>
void draw_battery_icon(Canvas& canvas, int x, int y, const char* const (&rows)[H], int w,
                       int percent, bool charging) {
  int min_x = w;
  int max_x = -1;
  for (int row = 0; row < static_cast<int>(H); ++row) {
    for (int col = 0; col < w; ++col) {
      if (rows[row][col] == 'G') {
        min_x = std::min(min_x, col);
        max_x = std::max(max_x, col);
      }
    }
  }
  const int span = std::max(1, max_x - min_x);
  const int lit_to = percent <= 0 ? min_x - 1 : min_x + span * std::clamp(percent, 0, 100) / 100;
  for (int row = 0; row < static_cast<int>(H); ++row) {
    for (int col = 0; col < w; ++col) {
      const char c = rows[row][col];
      if (c == '.') continue;
      const uint8_t color = c == 'W'   ? 11
                            : c == 'B' ? 3
                                       : (col <= lit_to ? (charging ? 4 : 11) : 3);
      canvas.pixel(x + col, y + row, color);
    }
  }
}

void draw_right_text(Canvas& canvas, int left, int right, int y, std::string text,
                     std::uint8_t color) {
  const int max_chars = std::max(0, (right - left) / Canvas::kCellW);
  if (static_cast<int>(text.size()) > max_chars) {
    text = text.substr(text.size() - static_cast<std::size_t>(max_chars));
  }
  const int width = static_cast<int>(text.size()) * Canvas::kCellW;
  const int x = std::max(left, right - width);
  canvas.draw_text(x, y, text, color, right);
}

}  // namespace

void Ui::header(const UiState& state) {
  canvas_.clear(0);
  // Stop one pixel above the page so the navy does not form a seam.
  canvas_.fill(0, 0, 240, 21, 3);

  // Raised tab strip, flush with the page below.
  canvas_.fill(0, 1, 70, 21, 8);
  canvas_.hline(0, 1, 70, 11);
  canvas_.vline(0, 1, 21, 11);
  canvas_.hline(0, 21, 70, 9);
  canvas_.vline(69, 1, 21, 9);
  canvas_.fill(1, 2, 68, 19, 0);

  const int pane = static_cast<int>(state.pane);
  draw_tab_glyph(canvas_, 4, 4, kLibraryTab, 11, pane == 0);
  draw_tab_glyph(canvas_, 17, 4, kDevTab, 13, pane == 1);
  draw_tab_glyph(canvas_, 32, 4, kSystemTab, 13, pane == 2);
  draw_tab_glyph(canvas_, 47, 4, kRouteTab, 12, pane == 3);
  draw_tab_glyph(canvas_, 60, 4, kClockTab, 9, pane == 4);

  uint8_t bpm_color = state.clock_running ? 15 : 4;
  if (state.clock_running && state.beat_phase < 6) bpm_color = 11;
  draw_right_text(canvas_, 71, 102, 2, std::to_string(state.bpm), bpm_color);

  std::string network = state.ssid;
  uint8_t network_color = 2;
  switch (state.wifi_state) {
    case platform::WifiState::Connected:
      if (network.empty()) network = "WIFI";
      network_color = 11;
      break;
    case platform::WifiState::Connecting:
      if (network.empty()) network = "CONNECTING";
      network_color = 2;  // dim grey while association is in progress
      break;
    case platform::WifiState::Failed:
      if (network.empty()) network = "WIFI ERROR";
      network_color = 6;
      break;
    default:
      if (network.empty()) network = "NO WIFI";
      network_color = 2;
      break;
  }
  draw_right_text(canvas_, 104, 202, 2, network, network_color);

  draw_wifi_icon(canvas_, 204, 2, kWifiIcon, 11, state.wifi, state.rssi);
  draw_sd_icon(canvas_, 217, 2, kSdIcon, 9, state.sd_mounted);
  draw_battery_icon(canvas_, 228, 2, kBatteryIcon, 11, state.battery, state.battery_charging);
}

void Ui::devices(const UiState& state, int, int rows) {
  canvas_.text(0, 2, "ID FIRMWARE            CLK", 2);
  const int total = static_cast<int>(state.midi.size() + state.msc.size());
  if (total == 0) {
    canvas_.text(1, 4, "No USB-MIDI devices.", 1);
    canvas_.text(1, 6, "Use a self-powered hub:", 2);
    canvas_.text(1, 7, "the USB-C port does not supply", 2);
    canvas_.text(1, 8, "VBUS.", 2);
    return;
  }
  const int first = std::max(0, state.selection - rows + 1);
  for (int row = 0; row < rows && first + row < total; ++row) {
    const int index = first + row;
    const bool selected = index == state.selection;
    const int y = 3 + row;
    if (selected) canvas_.fill(0, y * 8, 232, 8, 7);
    canvas_.text(0, y, selected ? ">" : " ", 2);
    if (index < static_cast<int>(state.midi.size())) {
      const auto& port = state.midi[static_cast<std::size_t>(index)];
      std::string label = port.identity.empty() ? ("MIDI " + port.path) : port.identity;
      canvas_.textf(1, y, selected ? 11 : 1, "%u %s", port.slot, trim(label, 18).c_str());
      if (port.ota) canvas_.text(25, y, "OTA", 5);
      if (port.claimed) canvas_.text(25, y, "BUSY", 6);
      canvas_.text(30, y, state.clock_mask & (1u << port.slot) ? "[x]" : "[ ]", selected ? 11 : 2);
    } else {
      const auto& port = state.msc[static_cast<std::size_t>(index - state.midi.size())];
      canvas_.textf(1, y, selected ? 11 : 1, "M%u ROM %s", port.slot, port.path.c_str());
      canvas_.text(30, y, "WL82", 5);
    }
  }
  scrollbar(first, rows, total, 33, 66);
}

void Ui::routes(const UiState& state, int, int rows) {
  canvas_.text(0, 2, "SOURCE        -> SINKS", 2);
  const int total = static_cast<int>(state.midi.size());
  if (total == 0) {
    canvas_.text(1, 4, "No MIDI sources.", 1);
    return;
  }
  const int first = std::max(0, state.selection - rows + 1);
  for (int row = 0; row < rows && first + row < total; ++row) {
    const int index = first + row;
    const auto& port = state.midi[static_cast<std::size_t>(index)];
    const bool selected = index == state.selection;
    const int y = 3 + row;
    if (selected) canvas_.fill(0, y * 8, 232, 8, 7);
    std::string label = port.identity.empty() ? ("MIDI " + port.path) : port.identity;
    canvas_.textf(0, y, selected ? 11 : 1, "%c%u %s", selected ? '>' : ' ', port.slot,
                  trim(label, 12).c_str());
    canvas_.text(17, y, "->", selected ? 11 : 2);
    char links[24]{};
    int at = 0;
    for (uint8_t sink = 0; sink < static_cast<uint8_t>(state.midi.size()); ++sink) {
      if (index < static_cast<int>(state.routes.size()) &&
          (state.routes[static_cast<std::size_t>(index)] & (1u << sink)) != 0) {
        at += std::snprintf(links + at, sizeof(links) - static_cast<std::size_t>(at), "%u ", sink);
      }
    }
    canvas_.text(20, y, at == 0 ? "none" : links, selected ? 11 : (at == 0 ? 2 : 4));
  }
  scrollbar(first, rows, total, 33, 66);
}

void Ui::clock(const UiState& state, int, int) {
  canvas_.fill(0, 22, 160, 11, 8);
  canvas_.hline(0, 22, 160, 11);
  canvas_.vline(0, 22, 11, 11);
  canvas_.hline(0, 32, 160, 9);
  canvas_.vline(159, 22, 11, 9);
  canvas_.textf(1, 2, 1, "%3u BPM", state.bpm);
  canvas_.text(13, 2, state.clock_running ? "RUN" : "STOP", state.clock_running ? 4 : 6);

  const int bar = static_cast<int>((state.beat_phase % 24) * 30 / 24);
  canvas_.fill(0, 37, 180, 3, 8);
  canvas_.fill(0, 37, bar * 6, 3, 4);

  canvas_.text(0, 4, "Targets:", 2);
  canvas_.text_clipped(9, 4, 30, state.clock_targets.empty() ? "none" : state.clock_targets, 1);

  const int total = static_cast<int>(state.midi.size());
  constexpr int kListRows = 4;
  const int first = std::max(0, state.selection - kListRows + 1);
  for (int row = 0; row < kListRows && first + row < total; ++row) {
    const int index = first + row;
    const auto& port = state.midi[static_cast<std::size_t>(index)];
    const bool selected = index == state.selection;
    const int y = 5 + row;
    if (selected) canvas_.fill(0, y * 11, 232, 11, 7);
    std::string label = port.identity.empty() ? ("MIDI " + port.path) : port.identity;
    canvas_.textf(0, y, selected ? 11 : 1, "%c[%c] %u %s", selected ? '>' : ' ',
                  checkbox(state.clock_mask & (1u << index)), port.slot, trim(label, 18).c_str());
  }
  scrollbar(first, kListRows, total, 55, 44);
}

void Ui::library(const UiState& state, int, int rows) {
  const int local_count = static_cast<int>(state.local.size());
  const int total = local_count + static_cast<int>(state.catalog.size());
  if (state.editing_filter) {
    canvas_.fill(0, 22, 240, 11, 1);
    canvas_.textf(0, 2, 0, "/%s_", state.filter.c_str());
  } else {
    if (!state.sd_mounted) {
      canvas_.textf(0, 2, 2, "LIB %zu SD NO %s %s", state.catalog_total,
                    state.device_filter.c_str(), state.chip_filter.c_str());
    } else if (state.local_total > state.local.size()) {
      canvas_.textf(0, 2, 2, "LIB %zu SD %zu/%zu %s %s", state.catalog_total, state.local.size(),
                    state.local_total, state.device_filter.c_str(), state.chip_filter.c_str());
    } else {
      canvas_.textf(0, 2, 2, "LIB %zu SD ok %s %s", state.catalog_total,
                    state.device_filter.c_str(), state.chip_filter.c_str());
    }
    if (!state.filter.empty()) {
      canvas_.text_clipped(30, 2, 10, "/" + state.filter, 5);
    }
  }
  if (total == 0) {
    canvas_.text(1, 4, "No packages or catalog.", 1);
    canvas_.text(1, 5, "Put .fwsc files on the card", 2);
    canvas_.text(1, 6, "or use the desktop app.", 2);
    return;
  }
  const int first = std::clamp(state.selection - rows / 2, 0, std::max(0, total - rows));
  for (int row = 0; row < rows && first + row < total; ++row) {
    const int index = first + row;
    const bool selected = index == state.selection;
    const int y = 3 + row;
    if (selected) canvas_.fill(0, y * 11, 232, 11, 7);
    if (index < local_count) {
      const auto& local = state.local[static_cast<std::size_t>(index)];
      canvas_.text(0, y, selected ? ">" : "*", selected ? 11 : 4);
      canvas_.text_clipped(1, y, 18, local.name, selected ? 11 : 1);
      canvas_.text(20, y, "SD", selected ? 11 : 4);
      canvas_.text_clipped(23, y, 15, local.product, selected ? 11 : 2);
    } else {
      const auto& entry = state.catalog[static_cast<std::size_t>(index - local_count)];
      const std::size_t catalog_index = static_cast<std::size_t>(index - local_count);
      const bool installed =
          catalog_index < state.catalog_installed.size() && state.catalog_installed[catalog_index];
      canvas_.text(0, y, selected ? ">" : " ", 2);
      canvas_.text_clipped(1, y, 17, entry.name, selected ? 11 : 1);
      canvas_.textf(19, y, selected ? 11 : 2, "%s", trim(entry.version, 7).c_str());
      canvas_.text(26, y, installed ? "SD" : "--", selected ? 11 : (installed ? 4 : 2));
      canvas_.text(28, y, entry.device == catalog::Device::Smk37 ? "SMK" : "FM1",
                   selected ? 11 : 5);
    }
  }
  scrollbar(first, rows, total, 33, rows * 11);
  canvas_.hline(0, 88, 240, 9);

  const auto* entry = [&]() -> const catalog::Entry* {
    if (state.selection < local_count) return nullptr;
    const auto index = static_cast<std::size_t>(state.selection - local_count);
    return index < state.catalog.size() ? &state.catalog[index] : nullptr;
  }();
  if (entry != nullptr) {
    canvas_.text_clipped(0, 8, 40, entry->name + "  " + entry->version, 1);
    canvas_.text_clipped(0, 9, 40, entry->author + "  " + entry->tags, 2);
  } else if (state.selection < local_count) {
    const auto& local = state.local[static_cast<std::size_t>(state.selection)];
    canvas_.text_clipped(
        0, 8, 40,
        local.name + "  " + local.product + "  " + std::to_string(local.size / 1024) + "K", 1);
    const std::string detail = !local.metadata.author.empty()
                                   ? local.metadata.author + "  " + local.metadata.tags
                                   : local.path;
    canvas_.text_clipped(0, 9, 40, detail, 2);
  }
}

void Ui::system(const UiState& state) {
  canvas_.fill(0, 22, 240, 11, 3);
  canvas_.text(0, 2, "SYSTEM", 11);
  canvas_.text(0, 3, "NAME", 2);
  canvas_.text_clipped(12, 3, 27, state.device_name.empty() ? "Cardputer" : state.device_name, 1);
  canvas_.text(0, 4, "SD CARD", 2);
  canvas_.textf(12, 4, state.sd_mounted ? 4 : 6, "%s",
                state.sd_mounted ? "mounted at /sdcard" : "NOT MOUNTED");

  const char* wifi = "unconfigured";
  switch (state.wifi_state) {
    case platform::WifiState::Connected:
      wifi = "connected";
      break;
    case platform::WifiState::Connecting:
      wifi = "connecting";
      break;
    case platform::WifiState::Failed:
      wifi = "failed";
      break;
    default:
      break;
  }
  canvas_.text(0, 5, "WI-FI", 2);
  canvas_.text(12, 5, wifi, state.wifi_state == platform::WifiState::Connected ? 4 : 1);
  canvas_.text(0, 6, "NETWORK", 2);
  canvas_.text_clipped(12, 6, 27, state.ssid.empty() ? "(none)" : state.ssid, 1);
  canvas_.text(0, 7, "IP", 2);
  canvas_.text(12, 7, state.ip.empty() ? "(no address)" : state.ip, 1);
  canvas_.text(0, 8, "SIGNAL", 2);
  if (state.rssi > -127) {
    const int percent = std::clamp((state.rssi + 100) * 2, 0, 100);
    canvas_.textf(12, 8, 1, "%d dBm  %d%%", state.rssi, percent);
  } else {
    canvas_.text(12, 8, "(not connected)", 2);
  }
  canvas_.text(0, 9, "BATTERY", 2);
  if (state.battery >= 0)
    canvas_.textf(12, 9, 1, "%d%%", state.battery);
  else
    canvas_.text(12, 9, "unknown", 2);
  canvas_.text(0, 10, "W wifi  N rename", 5);
}

void Ui::flash(const UiState& state) {
  if (!state.flash.running && !state.flash.error) return;
  canvas_.fill(0, 22, 240, 88, 0);
  canvas_.frame(0, 22, 240, 88, state.flash.error ? 6 : 3);
  canvas_.fill(1, 23, 238, 11, state.flash.error ? 6 : 3);
  canvas_.text(1, 2, state.flash.error ? "FLASH FAILED" : "FLASH", 11);
  canvas_.text_clipped(1, 3, 38, state.flash.step, 1);
  const int percent = std::clamp(state.flash.percent, 0, 100);
  canvas_.fill(6, 46, 228, 5, 8);
  canvas_.fill(6, 46, 228 * percent / 100, 5, 4);
  canvas_.textf(36, 4, 1, "%3d%%", percent);
  int row = 6;
  const int lines = std::min<int>(4, static_cast<int>(state.flash.log.size()));
  for (int i = 0; i < lines; ++i) {
    canvas_.text_clipped(1, row++, 38, state.flash.log[state.flash.log.size() - lines + i], 2);
  }
}

void Ui::popup(const UiState& state) {
  if (!state.popup) return;
  canvas_.fill(0, 22, 240, 88, 0);
  canvas_.frame(0, 22, 240, 88, 3);
  canvas_.fill(1, 23, 238, 11, 3);
  canvas_.text_clipped(1, 2, 38, state.popup_title, 11);
  constexpr int kPopupRows = 6;
  const int total = static_cast<int>(state.popup_items.size());
  const int first =
      std::clamp(state.popup_selection - kPopupRows / 2, 0, std::max(0, total - kPopupRows));
  for (int row = 0; row < kPopupRows && first + row < total; ++row) {
    const int index = first + row;
    const bool selected = index == state.popup_selection;
    const int y = 3 + row;
    if (selected) canvas_.fill(0, y * 11, 232, 11, 7);
    canvas_.text_clipped(1, y, 38, state.popup_items[static_cast<std::size_t>(index)],
                         selected ? 11 : 1);
  }
  scrollbar(first, kPopupRows, total, 33, 66);
}

void Ui::scrollbar(int first, int visible, int total, int y, int height) {
  if (total <= visible || visible <= 0) return;
  constexpr int x = 232;
  constexpr int w = 8;
  canvas_.fill(x, y, w, height, 8);
  canvas_.frame(x, y, w, height, 9);
  for (int i = 0; i < 3; ++i) {
    canvas_.hline(x + 3 - i, y + 2 + i, 1 + i * 2, 1);
    canvas_.hline(x + 3 - i, y + height - 3 - i, 1 + i * 2, 1);
  }
  const int track_y = y + 7;
  const int track_h = height - 14;
  const int thumb = std::max(4, track_h * visible / std::max(1, total));
  const int thumb_y = track_y + (track_h - thumb) * first / std::max(1, total - visible);
  canvas_.fill(x + 2, thumb_y, w - 4, thumb, 7);
}

void Ui::hoard(const UiState& state) {
  if (!state.hoard_active) return;
  canvas_.clear(0);
  canvas_.fill(0, 0, 240, 11, 3);
  canvas_.text(0, 0, "HOARD", 11);
  const int overall =
      state.hoard_total == 0 ? 0 : static_cast<int>(state.hoard_done * 100 / state.hoard_total);
  canvas_.textf(30, 0, 11, "%zu/%zu", state.hoard_done, state.hoard_total);
  canvas_.text(0, 2, overall == 0 ? "Reading local catalog..." : "Downloading all firmware", 1);
  canvas_.fill(5, 33, 230, 10, 8);
  if (overall > 0) canvas_.fill(5, 33, 230 * overall / 100, 10, 4);
  canvas_.textf(36, 3, 1, "%3d%%", overall);
  canvas_.text_clipped(0, 5, 40, state.hoard_current, 1);
  canvas_.textf(0, 7, 2, "Current file: %3d%%", state.hoard_percent);
  canvas_.textf(0, 8, state.hoard_failed ? 6 : 2, "Failed: %zu", state.hoard_failed);
  canvas_.text(0, 9, "Do not power off", 6);
  canvas_.text(0, 10, "ESC cancels", 2);
}

void Ui::footer(const UiState& state) {
  if (!state.message.empty()) {
    canvas_.fill(0, 99, 240, 11, state.message_error ? 6 : 5);
    canvas_.text_clipped(0, 9, 40, state.message, state.message_error ? 11 : 1);
  }
  canvas_.fill(0, 110, 240, 22, 8);
  canvas_.hline(0, 110, 240, 11);
  canvas_.hline(0, 120, 240, 9);
  const char* hint1 = "";
  const char* hint2 = "";
  if (state.flash.running || state.flash.error) {
    hint1 = "FLASHING - WAIT";
    hint2 = "";
  } else if (state.popup) {
    if (!state.popup_hint.empty()) {
      hint1 = state.popup_hint.c_str();
      hint2 = "";
    } else {
      hint1 = "UP/DN select   ENTER choose";
      hint2 = "ESC cancel";
    }
  } else
    switch (state.pane) {
      case Pane::Devices:
        if (state.selection < static_cast<int>(state.midi.size())) {
          hint1 = "1 CLOCK 2 FLASH 3 DOWN";
          hint2 = "ENTER FLASH  TAB  Fn;/. SEL";
        } else if (state.selection < static_cast<int>(state.midi.size() + state.msc.size())) {
          hint1 = "1 ROM";
          hint2 = "ENTER ROM  TAB  Fn;/. SEL";
        } else {
          hint1 = "No device";
          hint2 = "TAB pane";
        }
        break;
      case Pane::Routes:
        hint1 = "1 ROUTE  2 CLEAR";
        hint2 = "ENTER ROUTE  TAB  Fn;/. SEL";
        break;
      case Pane::Clock:
        hint1 = "1 RUN  2 ALL  3 -BPM  4 +BPM";
        hint2 = "ENTER TARGET  TAB  Fn;/. SEL";
        break;
      case Pane::Library:
        if (state.editing_filter) {
          hint1 = "TYPE   ENTER apply   ESC cancel";
        } else {
          hint1 = "1 DEV 2 CHIP 3 FILTER 4 FLASH";
          hint2 = "5 HOARD  TAB  ENTER DL";
        }
        break;
      case Pane::System:
        hint1 = "1 WI-FI  2 NAME";
        hint2 = "TAB pane";
        break;
    }
  canvas_.text_clipped(0, 10, 40, hint1, 1);
  canvas_.text_clipped(0, 11, 40, hint2, 2);
}

void Ui::draw(const UiState& state) {
  header(state);
  switch (state.pane) {
    case Pane::Devices:
      devices(state, 3, 6);
      break;
    case Pane::Routes:
      routes(state, 3, 6);
      break;
    case Pane::Clock:
      clock(state, 0, 0);
      break;
    case Pane::Library:
      library(state, 3, 5);
      break;
    case Pane::System:
      system(state);
      break;
  }
  flash(state);
  popup(state);
  footer(state);
  hoard(state);
}

}  // namespace fm1::ui

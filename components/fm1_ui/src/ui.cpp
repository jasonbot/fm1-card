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

char checkbox(bool on) { return on ? 'x' : ' '; }

}  // namespace

void Ui::header(const UiState& state) {
  canvas_.clear(0);
  canvas_.fill(0, 0, 240, 11, 3);
  canvas_.text(0, 0, "FM1-CARD", 11);
  canvas_.text(9, 0, state.sd_mounted ? "SD" : "--", state.sd_mounted ? 11 : 6);
  canvas_.textf(14, 0, 11, "U%u", static_cast<unsigned>(state.midi.size()));
  const char* wifi = "----";
  switch (state.wifi_state) {
    case platform::WifiState::Connected: wifi = "WIFI"; break;
    case platform::WifiState::Connecting: wifi = "WIFI.."; break;
    case platform::WifiState::Failed: wifi = "WIFI!"; break;
    case platform::WifiState::Unconfigured: wifi = "----"; break;
  }
  canvas_.text(17, 0, wifi, 11);
  canvas_.textf(25, 0, 11, "%u", state.bpm);
  canvas_.text(29, 0, "BPM", 11);
  if (state.battery >= 0) canvas_.textf(35, 0, 11, "%3d%%", state.battery);
}

void Ui::button(int x, int y, int w, int h, std::string_view label, bool active) {
  canvas_.fill(x, y, w, h, active ? 7 : 8);
  if (!active) {
    canvas_.hline(x, y, w, 11);
    canvas_.vline(x, y, h, 11);
    canvas_.hline(x, y + h - 1, w, 9);
    canvas_.vline(x + w - 1, y, h, 9);
  }
  const int cols = w / Canvas::kCellW;
  const int col = x / Canvas::kCellW + (cols - static_cast<int>(label.size())) / 2;
  canvas_.text(col, y / Canvas::kCellH, label, active ? 11 : 1);
}

void Ui::tabs(const UiState& state) {
  static constexpr const char* kNames[4] = {"DEV", "ROUTE", "CLOCK", "LIB"};
  constexpr int kWidth = 58;
  for (int i = 0; i < 4; ++i) {
    button(i * 61, 11, kWidth, 11, kNames[i], static_cast<int>(state.pane) == i);
  }
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
      canvas_.text(30, y, state.clock_mask & (1u << port.slot) ? "[x]" : "[ ]",
                   selected ? 11 : 2);
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
        at += std::snprintf(links + at, sizeof(links) - static_cast<std::size_t>(at), "%u ",
                            sink);
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
                  checkbox(state.clock_mask & (1u << index)), port.slot,
                  trim(label, 18).c_str());
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
      canvas_.textf(0, 2, 2, "LIB %zu SD %zu/%zu %s %s", state.catalog_total,
                    state.local.size(), state.local_total, state.device_filter.c_str(),
                    state.chip_filter.c_str());
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
    canvas_.text(1, 6, "or refresh with R.", 2);
    return;
  }
  const int first = std::max(0, state.selection - rows + 1);
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
      canvas_.text(0, y, selected ? ">" : " ", 2);
      canvas_.text_clipped(1, y, 17, entry.name, selected ? 11 : 1);
      canvas_.textf(19, y, selected ? 11 : 2, "%s", trim(entry.version, 7).c_str());
      canvas_.text(28, y, entry.device == catalog::Device::Smk37 ? "SMK" : "FM1",
                   selected ? 11 : 5);
    }
  }
  scrollbar(first, rows, total, 33, rows * 11);

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
    canvas_.text_clipped(0, 8, 40,
                         local.name + "  " + local.product + "  " +
                             std::to_string(local.size / 1024) + "K",
                         1);
    canvas_.text_clipped(0, 9, 40, local.path, 2);
  }
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
    canvas_.text_clipped(1, row++, 38,
                         state.flash.log[state.flash.log.size() - lines + i], 2);
  }
}

void Ui::popup(const UiState& state) {
  if (!state.popup) return;
  canvas_.fill(0, 22, 240, 88, 0);
  canvas_.frame(0, 22, 240, 88, 3);
  canvas_.fill(1, 23, 238, 11, 3);
  canvas_.text_clipped(1, 2, 38, state.popup_title, 11);
  const int first = std::max(0, state.popup_selection - 5);
  for (int row = 0; row < 6 && first + row < static_cast<int>(state.popup_items.size());
       ++row) {
    const int index = first + row;
    const bool selected = index == state.popup_selection;
    const int y = 3 + row;
    if (selected) canvas_.fill(0, y * 11, 232, 11, 7);
    canvas_.text_clipped(1, y, 38, state.popup_items[static_cast<std::size_t>(index)],
                         selected ? 11 : 1);
  }
  scrollbar(first, 6, static_cast<int>(state.popup_items.size()), 33, 66);
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
  const int overall = state.hoard_total == 0
                          ? 0
                          : static_cast<int>(state.hoard_done * 100 / state.hoard_total);
  canvas_.textf(30, 0, 11, "%zu/%zu", state.hoard_done, state.hoard_total);
  canvas_.text(0, 2, overall == 0 ? "Refreshing catalog..." : "Downloading all firmware",
               1);
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
  switch (state.pane) {
    case Pane::Devices:
      hint1 = "Fn ;/. select  ENTER clock  F flash";
      hint2 = "M mode  TAB pane";
      break;
    case Pane::Routes:
      hint1 = "Fn ;/. select  ENTER targets";
      hint2 = "DEL clear  TAB pane";
      break;
    case Pane::Clock:
      hint1 = "SPACE run  +/- bpm  T all";
      hint2 = "Fn ;/. select  ENTER target";
      break;
    case Pane::Library:
      if (state.editing_filter) {
        hint1 = "TYPE   ENTER apply   ESC cancel";
      } else {
        hint1 = "Fn ;/. select  Ctrl ;/. page";
        hint2 = "T dev C chip H hoard /filt R rel";
      }
      break;
  }
  canvas_.text_clipped(0, 10, 40, hint1, 1);
  canvas_.text_clipped(0, 11, 40, hint2, 2);
}

void Ui::draw(const UiState& state) {
  header(state);
  tabs(state);
  switch (state.pane) {
    case Pane::Devices: devices(state, 3, 6); break;
    case Pane::Routes: routes(state, 3, 6); break;
    case Pane::Clock: clock(state, 0, 0); break;
    case Pane::Library: library(state, 3, 5); break;
  }
  flash(state);
  popup(state);
  footer(state);
  hoard(state);
}

}  // namespace fm1::ui

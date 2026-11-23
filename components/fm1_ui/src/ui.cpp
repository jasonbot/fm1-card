#include "fm1/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace fm1::ui {
namespace {

std::string trim(std::string_view text, std::size_t width) {
  if (text.size() <= width) return std::string(text);
  if (width <= 1) return std::string(width, ' ');
  return std::string(text.substr(0, width - 2)) + "..";
}

char checkbox(bool on) { return on ? 'x' : ' '; }

}  // namespace

void Ui::header(const UiState& state) {
  canvas_.clear(0);
  canvas_.fill(0, 0, 240, 8, 3);
  canvas_.text(0, 0, "FM1-CARD", 0);
  const char* wifi = "----";
  switch (state.wifi_state) {
    case platform::WifiState::Connected: wifi = "WIFI"; break;
    case platform::WifiState::Connecting: wifi = "WIFI.."; break;
    case platform::WifiState::Failed: wifi = "WIFI!"; break;
    case platform::WifiState::Unconfigured: wifi = "----"; break;
  }
  canvas_.textf(10, 0, 0, "USB%u", static_cast<unsigned>(state.midi.size()));
  canvas_.textf(16, 0, 0, "%s", wifi);
  canvas_.textf(28, 0, 0, "%u BPM", state.bpm);
  if (state.battery >= 0) {
    canvas_.textf(35, 0, 0, "%3d%%", state.battery);
  }
}

void Ui::tabs(const UiState& state) {
  static constexpr const char* kNames[4] = {"1 DEV", "2 ROUTE", "3 CLOCK", "4 LIB"};
  int x = 0;
  for (int i = 0; i < 4; ++i) {
    const bool active = static_cast<int>(state.pane) == i;
    const std::string label = kNames[i];
    if (active) canvas_.fill(x, 8, static_cast<int>(label.size()) * 6, 8, 1);
    canvas_.text(x / 6, 1, label, active ? 0 : 2);
    x += static_cast<int>(label.size()) * 6 + 6;
  }
}

void Ui::devices(const UiState& state, int, int rows) {
  const int total = static_cast<int>(state.midi.size() + state.msc.size());
  if (total == 0) {
    canvas_.text(1, 3, "No USB-MIDI devices.", 1);
    canvas_.text(1, 5, "Use a self-powered hub:", 2);
    canvas_.text(1, 6, "the USB-C port does not", 2);
    canvas_.text(1, 7, "supply VBUS.", 2);
    return;
  }
  const int first = std::max(0, state.selection - rows + 1);
  for (int row = 0; row < rows && first + row < total; ++row) {
    const int index = first + row;
    const bool selected = index == state.selection;
    const int y = 2 + row;
    if (selected) canvas_.fill(0, y * 8, 240, 8, 8);
    canvas_.text(0, y, selected ? ">" : " ", 3);
    if (index < static_cast<int>(state.midi.size())) {
      const auto& port = state.midi[static_cast<std::size_t>(index)];
      std::string label = port.identity.empty() ? ("MIDI " + port.path) : port.identity;
      canvas_.textf(1, y, selected ? 1 : 1, "%u %s", port.slot, trim(label, 19).c_str());
      if (port.ota) canvas_.text(22, y, "OTA", 5);
      if (port.claimed) canvas_.text(26, y, "BUSY", 6);
      canvas_.text(33, y, port.path.c_str(), 2);
    } else {
      const auto& port = state.msc[static_cast<std::size_t>(index - state.midi.size())];
      canvas_.textf(1, y, 1, "M%u ROM %s", port.slot, port.path.c_str());
      canvas_.text(22, y, "WL82", 5);
    }
  }
}

void Ui::routes(const UiState& state, int, int rows) {
  const int total = static_cast<int>(state.midi.size());
  if (total == 0) {
    canvas_.text(1, 3, "No MIDI sources.", 1);
    return;
  }
  const int first = std::max(0, state.selection - rows + 1);
  for (int row = 0; row < rows && first + row < total; ++row) {
    const int index = first + row;
    const auto& port = state.midi[static_cast<std::size_t>(index)];
    const bool selected = index == state.selection;
    const int y = 2 + row;
    if (selected) canvas_.fill(0, y * 8, 240, 8, 8);
    std::string label = port.identity.empty() ? ("MIDI " + port.path) : port.identity;
    canvas_.textf(0, y, selected ? 3 : 1, "%c%u %s", selected ? '>' : ' ', port.slot,
                  trim(label, 15).c_str());
    canvas_.text(20, y, "->", 2);
    char links[24]{};
    int at = 0;
    for (uint8_t sink = 0; sink < static_cast<uint8_t>(state.midi.size()); ++sink) {
      if (index < static_cast<int>(state.routes.size()) &&
          (state.routes[static_cast<std::size_t>(index)] & (1u << sink)) != 0) {
        at += std::snprintf(links + at, sizeof(links) - static_cast<std::size_t>(at), "%u ",
                            sink);
      }
    }
    canvas_.text(23, y, at == 0 ? "none" : links, at == 0 ? 2 : 4);
  }
}

void Ui::clock(const UiState& state, int, int rows) {
  canvas_.fill(0, 16, 240, 16, 8);
  canvas_.textf(1, 2, 1, "%3u BPM", state.bpm);
  canvas_.text(13, 2, state.clock_running ? "RUN" : "STOP", state.clock_running ? 4 : 6);
  const int bar = static_cast<int>((state.beat_phase % 24) * 10 / 24);
  canvas_.text(1, 3, "|", 2);
  canvas_.fill(18 + bar * 4, 24, 4, 4, 4);
  canvas_.text(1, 5, "Targets:", 2);
  canvas_.text(10, 5, state.clock_targets.empty() ? "none" : state.clock_targets, 5);

  const int total = static_cast<int>(state.midi.size());
  const int first = std::max(0, state.selection - rows + 2);
  for (int row = 0; row < rows - 2 && first + row < total; ++row) {
    const int index = first + row;
    const auto& port = state.midi[static_cast<std::size_t>(index)];
    const bool selected = index == state.selection;
    const int y = 7 + row;
    if (selected) canvas_.fill(0, y * 8, 240, 8, 8);
    std::string label = port.identity.empty() ? ("MIDI " + port.path) : port.identity;
    canvas_.textf(0, y, selected ? 3 : 1, "%c[%c] %u %s", selected ? '>' : ' ',
                  checkbox(state.clock_mask & (1u << index)), port.slot,
                  trim(label, 18).c_str());
  }
}

void Ui::library(const UiState& state, int, int rows) {
  const int local_count = static_cast<int>(state.local.size());
  const int total = local_count + static_cast<int>(state.catalog.size());
  canvas_.textf(0, 2, 2, "LIB %d  SD %s", static_cast<int>(state.catalog.size()),
                state.sd_mounted ? "ok" : "NO");
  if (state.editing_filter) {
    canvas_.fill(0, 24, 240, 8, 1);
    canvas_.textf(0, 3, 0, "/%s_", state.filter.c_str());
  } else if (!state.filter.empty()) {
    canvas_.textf(0, 3, 5, "/%s", state.filter.c_str());
  } else {
    canvas_.text(0, 3, "firmwares", 2);
  }
  if (total == 0) {
    canvas_.text(1, 5, "No packages or catalog.", 1);
    canvas_.text(1, 6, "Put .fwsc files on the card", 2);
    canvas_.text(1, 7, "or refresh with R.", 2);
    return;
  }
  const int first = std::max(0, state.selection - rows + 1);
  for (int row = 0; row < rows && first + row < total; ++row) {
    const int index = first + row;
    const bool selected = index == state.selection;
    const int y = 4 + row;
    if (selected) canvas_.fill(0, y * 8, 240, 8, 8);
    if (index < local_count) {
      const auto& local = state.local[static_cast<std::size_t>(index)];
      canvas_.text(0, y, selected ? ">" : "*", 4);
      canvas_.textf(1, y, selected ? 1 : 1, "%s", trim(local.product, 20).c_str());
      canvas_.text(25, y, "SD", 4);
    } else {
      const auto& entry = state.catalog[static_cast<std::size_t>(index - local_count)];
      canvas_.text(0, y, selected ? ">" : " ", 3);
      canvas_.textf(1, y, selected ? 1 : 1, "%s", trim(entry.name, 18).c_str());
      canvas_.textf(20, y, 2, "%s", trim(entry.version, 8).c_str());
      canvas_.text(30, y, entry.device == catalog::Device::Smk37 ? "SMK" : "FM1", 5);
    }
  }
  const auto* entry = [&]() -> const catalog::Entry* {
    if (state.selection < local_count) return nullptr;
    const auto index = static_cast<std::size_t>(state.selection - local_count);
    return index < state.catalog.size() ? &state.catalog[index] : nullptr;
  }();
  if (entry != nullptr) {
    canvas_.text_clipped(0, 13, 40, entry->author + "  " + entry->tags, 2);
  } else if (state.selection < local_count) {
    canvas_.text_clipped(0, 13, 40, state.local[static_cast<std::size_t>(state.selection)].path, 2);
  }
}

void Ui::flash(const UiState& state) {
  if (!state.flash.running && !state.flash.error) return;
  canvas_.fill(0, 16, 240, 96, 0);
  canvas_.frame(0, 16, 240, 96, state.flash.error ? 6 : 3);
  canvas_.text(1, 3, state.flash.error ? "FLASH FAILED" : "FLASH", state.flash.error ? 6 : 3);
  canvas_.text_clipped(1, 4, 38, state.flash.step, 1);
  const int percent = std::clamp(state.flash.percent, 0, 100);
  canvas_.fill(1 * 6, 5 * 8 + 2, 38 * 6, 4, 8);
  canvas_.fill(1 * 6, 5 * 8 + 2, (38 * 6 * percent) / 100, 4, 4);
  canvas_.textf(36, 5, 1, "%3d%%", percent);
  int row = 7;
  const int lines = std::min<int>(5, static_cast<int>(state.flash.log.size()));
  for (int i = 0; i < lines; ++i) {
    canvas_.text_clipped(1, row++, 38,
                         state.flash.log[state.flash.log.size() - lines + i], 2);
  }
}

void Ui::popup(const UiState& state) {
  if (!state.popup) return;
  canvas_.fill(0, 16, 240, 96, 0);
  canvas_.frame(0, 16, 240, 96, 3);
  canvas_.text(1, 3, state.popup_title, 3);
  const int first = std::max(0, state.popup_selection - 7);
  for (int row = 0; row < 8 && first + row < static_cast<int>(state.popup_items.size());
       ++row) {
    const int index = first + row;
    const bool selected = index == state.popup_selection;
    if (selected) canvas_.fill(0, (4 + row) * 8, 240, 8, 8);
    canvas_.text_clipped(1, 4 + row, 38, state.popup_items[static_cast<std::size_t>(index)],
                         selected ? 1 : 2);
  }
}

void Ui::footer(const UiState& state) {
  canvas_.fill(0, 120, 240, 15, 8);
  if (!state.message.empty()) {
    canvas_.text_clipped(0, 13, 40, state.message, state.message_error ? 6 : 5);
  }
  const char* hint = "";
  switch (state.pane) {
    case Pane::Devices:
      hint = "ENT clock  F flash  M mode";
      break;
    case Pane::Routes:
      hint = "ENT link  SPC toggle  DEL clear";
      break;
    case Pane::Clock:
      hint = "SPC run  +/- bpm  T targets";
      break;
    case Pane::Library:
      hint = state.editing_filter ? "TYPE  ENT apply  ESC cancel"
                                  : "/ filter  D download  F flash  R reload";
      break;
  }
  canvas_.text_clipped(0, 15, 40, hint, 2);
}

void Ui::draw(const UiState& state) {
  header(state);
  tabs(state);
  constexpr int kFirst = 2;
  constexpr int kRows = 11;
  switch (state.pane) {
    case Pane::Devices: devices(state, kFirst, kRows); break;
    case Pane::Routes: routes(state, kFirst, kRows); break;
    case Pane::Clock: clock(state, kFirst, kRows); break;
    case Pane::Library: library(state, kFirst, kRows); break;
  }
  flash(state);
  popup(state);
  footer(state);
}

}  // namespace fm1::ui

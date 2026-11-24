#pragma once

// Cardputer-Adv board support: ST7789V2 display, TCA8418 keyboard, SD card,
// battery. Everything here is ESP-IDF-only; the UI sees a canvas and key events.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fm1/result.hpp"

namespace fm1::platform {

struct Rgb565 {
  uint16_t value = 0;
};

class Display {
 public:
  static constexpr int kWidth = 240;
  static constexpr int kHeight = 135;

  Result<void> init();
  // Draws a fully converted rectangle. Blocks until the SPI transfer is done.
  Result<void> blit(int x, int y, int width, int height, std::span<const uint16_t> pixels);
  void set_backlight(bool on);
  [[nodiscard]] bool ready() const { return ready_; }

 private:
  Result<void> command(uint8_t command, std::span<const uint8_t> data = {});
  Result<void> set_window(int x, int y, int width, int height);

  void* io_ = nullptr;
  void* done_ = nullptr;
  bool ready_ = false;
};

enum class KeyKind : uint8_t { Press, Release };

struct KeyEvent {
  KeyKind kind = KeyKind::Press;
  char character = 0;  // shifted/printed character, or 0
  uint8_t code = 0;    // HID-style code for non-printing keys, or 0
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool fn = false;
};

class Keyboard {
 public:
  Result<void> init();
  // Non-blocking: returns one event per call.
  std::optional<KeyEvent> poll();
  [[nodiscard]] bool ready() const { return ready_; }

 private:
  Result<void> write(uint8_t reg, uint8_t value);
  Result<uint8_t> read(uint8_t reg);
  void apply(uint8_t event);

  void* bus_ = nullptr;
  void* device_ = nullptr;
  bool ready_ = false;
  std::optional<KeyEvent> next_event_;
  bool pressed_[4][14]{};
  bool shift_ = false;
  bool ctrl_ = false;
  bool alt_ = false;
  bool fn_ = false;
};

class Storage {
 public:
  Result<void> mount();
  [[nodiscard]] bool mounted() const { return mounted_; }
  [[nodiscard]] std::string path(std::string_view relative) const;
  Result<std::string> read_text(std::string_view relative, std::size_t limit = 16 * 1024) const;
  Result<void> write_text(std::string_view relative, std::string_view text) const;
  Result<void> ensure_dir(std::string_view relative) const;
  [[nodiscard]] uint64_t free_bytes() const;

 private:
  bool mounted_ = false;
};

class Battery {
 public:
  Result<void> init();
  // Percent, or -1 when unknown.
  [[nodiscard]] int percent() const;
  // Battery voltage in millivolts, or -1 when unknown.
  [[nodiscard]] int millivolts() const;

 private:
  void* adc_ = nullptr;
  bool ready_ = false;
};

}  // namespace fm1::platform

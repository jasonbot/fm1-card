#pragma once

// A 240x135 4-bpp console canvas. Text cells are 6x8 (40 columns x 16 rows),
// glyphs are the classic 5x7 font, colors are a 16-entry RGB565 palette.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fm1::platform {
class Display;
}

namespace fm1::ui {

struct Glyph {
  uint8_t advance = 0;
  uint8_t width = 0;
  uint8_t height = 0;
  int8_t top = 0;
  int8_t xoff = 0;
  std::array<uint8_t, 11> rows{};
};

extern const Glyph kGlyphs[95];

class Canvas {
 public:
  static constexpr int kWidth = 240;
  static constexpr int kHeight = 135;
  static constexpr int kCellW = 6;
  static constexpr int kCellH = 11;
  static constexpr int kCols = kWidth / kCellW;   // 40
  static constexpr int kRows = kHeight / kCellH;  // 12

  Canvas();
  void clear(uint8_t color = 0);
  void pixel(int x, int y, uint8_t color);
  void fill(int x, int y, int width, int height, uint8_t color);
  void frame(int x, int y, int width, int height, uint8_t color);
  void hline(int x, int y, int width, uint8_t color);
  void vline(int x, int y, int height, uint8_t color);
  void char_at(int col, int row, char c, uint8_t color);
  void draw_text(int x, int y, std::string_view text, uint8_t color, int max_x);
  void text(int col, int row, std::string_view text, uint8_t color);
  void textf(int col, int row, uint8_t color, const char* format, ...);
  void text_clipped(int col, int row, int width, std::string_view text, uint8_t color);

  [[nodiscard]] uint8_t at(int x, int y) const;
  [[nodiscard]] int columns() const { return kCols; }
  [[nodiscard]] int rows() const { return kRows; }

  // Present to a Display in 16-line bands. The callback receives an RGB565
  // rectangle; it must block until the transfer is done.
  using BlitFn = void (*)(void* context, int x, int y, int width, int height,
                          std::span<const uint16_t> pixels);
  void present(void* context, BlitFn blit);

 private:
  std::array<uint8_t, (kWidth * kHeight + 1) / 2> pixels_{};
  std::array<uint16_t, kWidth> palette_{};
  std::array<uint16_t, kWidth * 16> band_{};
};

}  // namespace fm1::ui

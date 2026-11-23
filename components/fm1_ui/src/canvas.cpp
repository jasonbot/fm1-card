#include "fm1/ui/canvas.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace fm1::ui {

Canvas::Canvas() {
  palette_[0] = 0xE71C;   // 0  Palm light gray / background
  palette_[1] = 0x0000;   // 1  black text
  palette_[2] = 0x6B4D;   // 2  dim gray
  palette_[3] = 0x0010;   // 3  navy title bar
  palette_[4] = 0x0400;   // 4  green
  palette_[5] = 0xFC00;   // 5  orange
  palette_[6] = 0x9800;   // 6  red
  palette_[7] = 0x0008;   // 7  selection (deeper navy)
  palette_[8] = 0xC618;   // 8  button face / separator
  palette_[9] = 0x8410;   // 9  bevel shadow
  palette_[10] = 0xCE79;  // 10 pale blue
  palette_[11] = 0xFFFF;  // 11 white
  palette_[12] = 0xEF7D;  // 12 off-white
  palette_[13] = 0xB5B6;  // 13 mid gray
  palette_[14] = 0x39E7;  // 14 dark gray
  palette_[15] = 0x0000;  // 15 black
}

void Canvas::clear(uint8_t color) {
  const uint8_t packed = static_cast<uint8_t>((color & 0x0F) | ((color & 0x0F) << 4));
  pixels_.fill(packed);
}

void Canvas::pixel(int x, int y, uint8_t color) {
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
  const std::size_t index = static_cast<std::size_t>(y * kWidth + x);
  const std::size_t byte = index / 2;
  if ((index & 1) == 0) {
    pixels_[byte] = static_cast<uint8_t>((pixels_[byte] & 0xF0) | (color & 0x0F));
  } else {
    pixels_[byte] = static_cast<uint8_t>((pixels_[byte] & 0x0F) | ((color & 0x0F) << 4));
  }
}

uint8_t Canvas::at(int x, int y) const {
  if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return 0;
  const std::size_t index = static_cast<std::size_t>(y * kWidth + x);
  const uint8_t byte = pixels_[index / 2];
  return (index & 1) == 0 ? byte & 0x0F : byte >> 4;
}

void Canvas::fill(int x, int y, int width, int height, uint8_t color) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(kWidth, x + width);
  const int y1 = std::min(kHeight, y + height);
  for (int row = y0; row < y1; ++row) {
    for (int col = x0; col < x1; ++col) pixel(col, row, color);
  }
}

void Canvas::frame(int x, int y, int width, int height, uint8_t color) {
  hline(x, y, width, color);
  hline(x, y + height - 1, width, color);
  vline(x, y, height, color);
  vline(x + width - 1, y, height, color);
}

void Canvas::hline(int x, int y, int width, uint8_t color) {
  for (int i = 0; i < width; ++i) pixel(x + i, y, color);
}

void Canvas::vline(int x, int y, int height, uint8_t color) {
  for (int i = 0; i < height; ++i) pixel(x, y + i, color);
}

void Canvas::char_at(int col, int row, char c, uint8_t color) {
  if (col < 0 || col >= kCols || row < 0 || row >= kRows) return;
  const int x0 = col * kCellW;
  const int y0 = row * kCellH;
  const auto ch = static_cast<unsigned char>(c);
  const uint8_t* glyph = &kFont5x7[(ch < 32 || ch > 126 ? '?' : ch) * 5];
  for (int gx = 0; gx < 5; ++gx) {
    const uint8_t bits = glyph[gx];
    for (int gy = 0; gy < 7; ++gy) {
      if ((bits & (1u << gy)) != 0) pixel(x0 + gx, y0 + gy, color);
    }
  }
}

void Canvas::text(int col, int row, std::string_view text, uint8_t color) {
  for (std::size_t i = 0; i < text.size() && col + static_cast<int>(i) < kCols; ++i) {
    char_at(col + static_cast<int>(i), row, text[i], color);
  }
}

void Canvas::text_clipped(int col, int row, int width, std::string_view text, uint8_t color) {
  for (int i = 0; i < width && i < static_cast<int>(text.size()); ++i) {
    char_at(col + i, row, text[static_cast<std::size_t>(i)], color);
  }
}

void Canvas::textf(int col, int row, uint8_t color, const char* format, ...) {
  char buffer[96]{};
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  text(col, row, buffer, color);
}

void Canvas::present(void* context, BlitFn blit) {
  for (int y = 0; y < kHeight; y += 16) {
    const int height = std::min(16, kHeight - y);
    const std::size_t count = static_cast<std::size_t>(kWidth * height);
    for (int row = 0; row < height; ++row) {
      for (int col = 0; col < kWidth; ++col) {
        band_[static_cast<std::size_t>(row * kWidth + col)] =
            palette_[at(col, y + row) & 0x0F];
      }
    }
    blit(context, 0, y, kWidth, height, std::span<const uint16_t>(band_.data(), count));
  }
}

}  // namespace fm1::ui

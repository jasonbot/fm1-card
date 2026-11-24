#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "fm1/platform/board.hpp"
#include "fm1/result.hpp"

namespace fm1::platform {

struct Config {
  std::string device_name = "Cardputer";
  std::string theme_blue = "#000083";
  std::string ssid;
  std::string password;
};

Result<Config> load_config(Storage& storage);
Result<void> write_config_template(Storage& storage);
Result<void> save_config(Storage& storage, const Config& config);
// Load, mutate and save under one lock so concurrent config writers cannot
// lose each other's fields.
Result<Config> update_config(Storage& storage, const std::function<void(Config&)>& mutate);
// Trim edges, drop control characters, cap at 32 bytes, default "Cardputer".
std::string normalize_device_name(std::string_view name);

inline std::optional<uint16_t> parse_hex_color(std::string_view text) {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);
  if (text.size() != 6) return std::nullopt;
  uint32_t rgb = 0;
  for (const char c : text) {
    const int digit = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
    if (digit < 0) return std::nullopt;
    rgb = (rgb << 4) | static_cast<uint32_t>(digit);
  }
  const uint16_t r = static_cast<uint16_t>((rgb >> 16) & 0xFF);
  const uint16_t g = static_cast<uint16_t>((rgb >> 8) & 0xFF);
  const uint16_t b = static_cast<uint16_t>(rgb & 0xFF);
  return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

inline std::string format_hex_color(uint16_t color) {
  static constexpr char digits[] = "0123456789ABCDEF";
  const uint8_t r = static_cast<uint8_t>(((color >> 11) & 0x1F) * 255 / 31);
  const uint8_t g = static_cast<uint8_t>(((color >> 5) & 0x3F) * 255 / 63);
  const uint8_t b = static_cast<uint8_t>((color & 0x1F) * 255 / 31);
  std::string out = "#";
  for (const uint8_t value : {r, g, b}) {
    out.push_back(digits[value >> 4]);
    out.push_back(digits[value & 0x0F]);
  }
  return out;
}

}  // namespace fm1::platform

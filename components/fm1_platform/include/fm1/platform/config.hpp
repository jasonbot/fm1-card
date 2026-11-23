#pragma once

#include <string>
#include <string_view>

#include "fm1/platform/board.hpp"
#include "fm1/result.hpp"

namespace fm1::platform {

struct Config {
  std::string device_name = "Cardputer";
  std::string ssid;
  std::string password;
  std::string catalog_url = "https://www.jasonscheirer.com/firmware-zoo/firmwares.json";
};

Result<Config> load_config(Storage& storage);
Result<void> write_config_template(Storage& storage);
Result<void> save_config(Storage& storage, const Config& config);
// Trim edges, drop control characters, cap at 32 bytes, default "Cardputer".
std::string normalize_device_name(std::string_view name);

}  // namespace fm1::platform

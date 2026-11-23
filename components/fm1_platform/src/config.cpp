#include "fm1/platform/config.hpp"

#include <array>
#include <cstring>

#include <lwjson/lwjson.h>

namespace fm1::platform {
namespace {

constexpr const char* kConfigPath = "fm1-card/config.json";
constexpr const char* kTemplate =
    "{\n"
    "  \"device_name\": \"Cardputer\",\n"
    "  \"wifi\": { \"ssid\": \"\", \"password\": \"\" },\n"
    "  \"catalog\": \"https://www.jasonscheirer.com/firmware-zoo/firmwares.json\"\n"
    "}\n";

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
        break;
    }
  }
  return out;
}

std::string value_of(const lwjson_token_t* token) {
  if (token == nullptr) return {};
  size_t length = 0;
  const char* text = lwjson_get_val_string(token, &length);
  return text != nullptr ? std::string(text, length) : std::string{};
}

}  // namespace

std::string normalize_device_name(std::string_view name) {
  std::size_t begin = 0;
  std::size_t end = name.size();
  while (begin < end && (name[begin] == ' ' || name[begin] == '\t')) ++begin;
  while (end > begin && (name[end - 1] == ' ' || name[end - 1] == '\t')) --end;
  std::string out;
  out.reserve(32);
  for (std::size_t i = begin; i < end && out.size() < 32; ++i) {
    const char c = name[i];
    if (static_cast<unsigned char>(c) >= 32) out.push_back(c == ';' ? '-' : c);
  }
  return out.empty() ? std::string("Cardputer") : out;
}

Result<Config> load_config(Storage& storage) {
  Config config;
  auto text = storage.read_text(kConfigPath, 4096);
  if (!text) {
    // First boot: leave a template so the user can edit the card.
    (void)write_config_template(storage);
    return config;
  }
  std::array<lwjson_token_t, 64> tokens{};
  lwjson_t json{};
  if (lwjson_init(&json, tokens.data(), tokens.size()) != lwjsonOK) {
    return fail("cannot initialise config parser");
  }
  if (lwjson_parse(&json, text->c_str()) != lwjsonOK) {
    return fail("config.json is not valid JSON");
  }
  config.device_name = normalize_device_name(value_of(lwjson_find(&json, "device_name")));
  const auto* wifi = lwjson_find(&json, "wifi");
  if (wifi != nullptr) {
    config.ssid = value_of(lwjson_find_ex(&json, wifi, "ssid"));
    config.password = value_of(lwjson_find_ex(&json, wifi, "password"));
  }
  const auto url = value_of(lwjson_find(&json, "catalog"));
  if (!url.empty()) config.catalog_url = url;
  return config;
}

Result<void> write_config_template(Storage& storage) {
  (void)storage.ensure_dir("fm1-card");
  return storage.write_text(kConfigPath, kTemplate);
}

Result<void> save_config(Storage& storage, const Config& config) {
  if (!storage.mounted()) return fail("no SD card");
  (void)storage.ensure_dir("fm1-card");
  std::string text;
  text.reserve(512);
  text += "{\n  \"device_name\": \"";
  text += json_escape(normalize_device_name(config.device_name));
  text += "\",\n  \"wifi\": { \"ssid\": \"";
  text += json_escape(config.ssid);
  text += "\", \"password\": \"";
  text += json_escape(config.password);
  text += "\" },\n  \"catalog\": \"";
  text += json_escape(config.catalog_url);
  text += "\"\n}\n";
  return storage.write_text(kConfigPath, text);
}

}  // namespace fm1::platform

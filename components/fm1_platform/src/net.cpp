#include "fm1/platform/net.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "psa/crypto.h"
#include "nvs_flash.h"

namespace fm1::platform {
namespace {

constexpr const char* kTag = "fm1-net";
constexpr EventBits_t kWifiConnected = BIT0;
constexpr EventBits_t kIp4 = BIT1;
constexpr EventBits_t kFail = BIT2;

void wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
  auto* net = static_cast<Net*>(arg);
  auto group = static_cast<EventGroupHandle_t>(net->event_group());
  (void)net;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(group, kWifiConnected | kIp4);
    net->note_disconnected();
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(group, kWifiConnected | kIp4);
    net->note_connected();
  }
}

std::string hex(const uint8_t* bytes, std::size_t length);

Result<std::string> sha256_file(const std::string& path, std::size_t* size_out) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return fail("cannot open " + path);
  psa_crypto_init();
  psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
  psa_hash_setup(&sha, PSA_ALG_SHA_256);
  std::array<uint8_t, 4096> buffer{};
  std::size_t total = 0;
  for (;;) {
    const auto got = std::fread(buffer.data(), 1, buffer.size(), file);
    if (got == 0) break;
    total += got;
    psa_hash_update(&sha, buffer.data(), got);
  }
  std::fclose(file);
  std::array<uint8_t, 32> digest{};
  size_t digest_length = 0;
  psa_hash_finish(&sha, digest.data(), digest.size(), &digest_length);
  if (size_out != nullptr) *size_out = total;
  return hex(digest.data(), digest_length);
}

std::string hex(const uint8_t* bytes, std::size_t length) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0xF]);
  }
  return out;
}

}  // namespace

Result<void> Net::start(Storage& storage, const Config& config) {
  storage_ = &storage;
  catalog_url_ = config.catalog_url;
  ssid_ = config.ssid;
  configured_.store(!ssid_.empty());
  if (ssid_.empty()) {
    state_.store(WifiState::Unconfigured);
    ESP_LOGW(kTag, "no Wi-Fi SSID configured; SD-only mode");
    return {};
  }
  state_.store(WifiState::Connecting);
  event_group_ = xEventGroupCreate();
  if (event_group_ == nullptr) return fail("no Wi-Fi event group");

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    state_.store(WifiState::Failed);
    return fail("NVS init failed");
  }

  err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    state_.store(WifiState::Failed);
    return fail("netif init failed");
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    state_.store(WifiState::Failed);
    return fail("event loop failed");
  }
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init);
  if (err != ESP_OK) {
    state_.store(WifiState::Failed);
    return fail("Wi-Fi init failed");
  }
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, this, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, this, nullptr);

  wifi_config_t wifi{};
  std::strncpy(reinterpret_cast<char*>(wifi.sta.ssid), config.ssid.c_str(),
               sizeof(wifi.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi.sta.password), config.password.c_str(),
               sizeof(wifi.sta.password) - 1);
  wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    state_.store(WifiState::Failed);
    return fail("Wi-Fi mode failed");
  }
  err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
  if (err != ESP_OK) {
    state_.store(WifiState::Failed);
    return fail("Wi-Fi config failed");
  }
  err = esp_wifi_start();
  if (err != ESP_OK) {
    state_.store(WifiState::Failed);
    return fail("Wi-Fi start failed");
  }
  esp_wifi_set_ps(WIFI_PS_NONE);
  started_ = true;
  auto connected = connect_wifi();
  if (!connected) {
    state_.store(WifiState::Failed);
    std::lock_guard lock(error_lock_);
    wifi_error_ = connected.error().message;
    return connected;
  }
  state_.store(WifiState::Connected);
  return {};
}

Result<void> Net::connect_wifi() {
  auto group = static_cast<EventGroupHandle_t>(event_group_);
  if (group == nullptr) return fail("no event group");
  xEventGroupClearBits(group, kWifiConnected | kIp4 | kFail);
  esp_wifi_connect();
  const auto bits = xEventGroupWaitBits(group, kIp4 | kFail, pdFALSE, pdTRUE,
                                        pdMS_TO_TICKS(20'000));
  if ((bits & kIp4) == 0) {
    return fail("did not get an IP within 20 s (SSID \"" + ssid_ + "\")");
  }
  ESP_LOGI(kTag, "Wi-Fi connected");
  state_.store(WifiState::Connected);
  return {};
}

bool Net::wifi_connected() const {
  if (event_group_ == nullptr) return false;
  return (xEventGroupGetBits(static_cast<EventGroupHandle_t>(event_group_)) & kIp4) != 0;
}

std::string Net::wifi_error() const {
  std::lock_guard lock(error_lock_);
  return wifi_error_;
}

void Net::note_connected() {
  state_.store(WifiState::Connected);
  std::lock_guard lock(error_lock_);
  wifi_error_.clear();
}

void Net::note_disconnected() {
  if (configured_.load()) {
    state_.store(WifiState::Connecting);
  } else {
    state_.store(WifiState::Unconfigured);
  }
}

Result<void> Net::ensure_connected() {
  if (!configured_.load()) {
    return fail("Wi-Fi is not configured; edit fm1-card/config.json and reboot");
  }
  if (wifi_connected()) {
    state_.store(WifiState::Connected);
    return {};
  }
  if (!started_) {
    return fail("Wi-Fi is not initialized");
  }
  state_.store(WifiState::Connecting);
  auto connected = connect_wifi();
  if (!connected) {
    state_.store(WifiState::Failed);
    std::lock_guard lock(error_lock_);
    wifi_error_ = connected.error().message;
    return fail(wifi_error_);
  }
  state_.store(WifiState::Connected);
  return {};
}

void Net::stop() {
  if (started_) {
    esp_wifi_stop();
    esp_wifi_deinit();
    started_ = false;
  }
}

Result<void> Net::fetch(const std::string& url, const ProgressFn& progress,
                        const std::function<bool(std::span<const char>)>& sink) {
  if (!wifi_connected()) {
    return fail(wifi_configured() ? "Wi-Fi is still connecting; try again in a moment"
                                 : "Wi-Fi is not configured");
  }
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 30'000;
  config.buffer_size = 4096;
  config.buffer_size_tx = 1024;
  config.keep_alive_enable = false;
  auto* client = esp_http_client_init(&config);
  if (client == nullptr) return fail("HTTP client init failed");
  esp_http_client_set_header(client, "User-Agent", "fm1-card/0.1");
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return fail("HTTP open failed");
  }
  const int status = esp_http_client_fetch_headers(client);
  if (status < 0) {
    esp_http_client_cleanup(client);
    return fail("HTTP headers failed");
  }
  if (esp_http_client_get_status_code(client) < 200 ||
      esp_http_client_get_status_code(client) >= 300) {
    esp_http_client_cleanup(client);
    return fail("HTTP status " + std::to_string(esp_http_client_get_status_code(client)));
  }
  const int64_t length = esp_http_client_get_content_length(client);
  std::array<char, 4096> buffer{};
  std::size_t received = 0;
  bool keep_going = true;
  while (keep_going) {
    const int got = esp_http_client_read(client, buffer.data(), buffer.size());
    if (got < 0) {
      esp_http_client_cleanup(client);
      return fail("HTTP read failed");
    }
    if (got == 0) break;
    received += static_cast<std::size_t>(got);
    keep_going = sink(std::span<const char>(buffer.data(), got));
    if (progress) {
      progress(NetProgress{.received = received,
                           .total = length > 0 ? static_cast<std::size_t>(length) : 0,
                           .phase = "download"});
    }
  }
  if (!esp_http_client_is_complete_data_received(client)) {
    esp_http_client_cleanup(client);
    return fail("HTTP transfer was incomplete");
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return {};
}

Result<void> Net::refresh_catalog(const ProgressFn& progress) {
  if (storage_ == nullptr) return fail("no storage");
  if (!storage_->mounted()) return fail("no SD card mounted; insert the card and reboot");
  const auto log = [&](std::string_view text) {
    (void)storage_->write_text("fm1-card/logs/catalog.log", text);
  };
  log("started\n");
  auto connected = ensure_connected();
  if (!connected) {
    log("failed: " + connected.error().message + "\n");
    return connected;
  }
  (void)storage_->ensure_dir("fm1-card");
  const auto final_path = storage_->path("fm1-card/catalog.bin");
  const auto temp_path = final_path + ".tmp";
  std::FILE* file = std::fopen(temp_path.c_str(), "wb");
  if (file == nullptr) return fail("cannot create catalog temp file");

  std::size_t entries = 0;
  bool write_ok = true;
  catalog::StreamParser parser([&](catalog::Entry&& entry) {
    const auto record = catalog::encode_record(entry);
    const uint32_t length = static_cast<uint32_t>(record.size());
    const uint8_t header[4]{static_cast<uint8_t>(length),
                            static_cast<uint8_t>(length >> 8),
                            static_cast<uint8_t>(length >> 16),
                            static_cast<uint8_t>(length >> 24)};
    if (std::fwrite(header, 1, 4, file) != 4 ||
        std::fwrite(record.data(), 1, record.size(), file) != record.size()) {
      write_ok = false;
    }
    ++entries;
  });

  auto result = fetch(catalog_url_, progress, [&](std::span<const char> chunk) {
    parser.feed(chunk);
    return true;
  });
  if (!result) {
    std::fclose(file);
    std::remove(temp_path.c_str());
    log("failed: " + result.error().message + "\n");
    return result;
  }
  auto finish = parser.finish();
  std::fflush(file);
  std::fclose(file);
  if (!write_ok) {
    std::remove(temp_path.c_str());
    log("failed: could not write the catalog cache\n");
    return fail("could not write the catalog cache");
  }
  if (!finish) {
    std::remove(temp_path.c_str());
    log("failed: " + finish.error().message + "\n");
    return finish;
  }
  std::remove(final_path.c_str());
  if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(temp_path.c_str());
    log("failed: cannot replace the catalog cache\n");
    return fail("cannot replace the catalog cache");
  }
  log("ok: " + std::to_string(entries) + " entries\n");
  ESP_LOGI(kTag, "catalog cached: %u entries", static_cast<unsigned>(entries));
  return {};
}

Result<void> Net::download(const catalog::Entry& entry, const std::string& destination,
                           const ProgressFn& progress) {
  if (storage_ == nullptr) return fail("no storage");
  if (!storage_->mounted()) return fail("no SD card mounted; insert the card and reboot");
  auto connected = ensure_connected();
  if (!connected) return connected;
  const auto temp_path = destination + ".tmp";
  std::FILE* file = std::fopen(temp_path.c_str(), "wb");
  if (file == nullptr) return fail("cannot create " + temp_path);

  psa_crypto_init();
  psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
  psa_hash_setup(&sha, PSA_ALG_SHA_256);
  std::size_t written = 0;
  auto result = fetch(catalog::package_url(entry), progress, [&](std::span<const char> chunk) {
    if (std::fwrite(chunk.data(), 1, chunk.size(), file) != chunk.size()) return false;
    psa_hash_update(&sha, reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size());
    written += chunk.size();
    return true;
  });
  std::fflush(file);
  std::fclose(file);
  if (!result) {
    psa_hash_abort(&sha);
    std::remove(temp_path.c_str());
    return result;
  }
  std::array<uint8_t, 32> digest{};
  size_t digest_length = 0;
  psa_hash_finish(&sha, digest.data(), digest.size(), &digest_length);
  const auto actual = hex(digest.data(), digest.size());
  if (!entry.sha256.empty()) {
    std::string expected = entry.sha256;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (actual != expected) {
      std::remove(temp_path.c_str());
      return fail("SHA-256 mismatch for " + entry.name);
    }
  }
  std::remove(destination.c_str());
  if (std::rename(temp_path.c_str(), destination.c_str()) != 0) {
    std::remove(temp_path.c_str());
    return fail("cannot save " + destination);
  }
  const std::string meta_path = destination + ".meta";
  if (std::FILE* meta = std::fopen(meta_path.c_str(), "wb")) {
    std::fprintf(meta, "%s\n%s\n%zu\n", catalog::package_url(entry).c_str(),
                 actual.c_str(), written);
    std::fclose(meta);
  }
  return {};
}

Result<bool> Net::has_current(const catalog::Entry& entry, const std::string& destination) {
  std::FILE* meta = std::fopen((destination + ".meta").c_str(), "rb");
  if (meta == nullptr) return false;
  char url[512]{};
  char digest[128]{};
  unsigned long long size = 0;
  const bool read_ok = std::fgets(url, sizeof(url), meta) != nullptr &&
                       std::fgets(digest, sizeof(digest), meta) != nullptr &&
                       std::fscanf(meta, "%llu", &size) == 1;
  std::fclose(meta);
  if (!read_ok) return false;
  url[std::strcspn(url, "\r\n")] = '\0';
  digest[std::strcspn(digest, "\r\n")] = '\0';
  if (catalog::package_url(entry) != url) return false;
  std::size_t local_size = 0;
  auto local_digest = sha256_file(destination, &local_size);
  if (!local_digest) return false;
  return local_size == size && *local_digest == digest;
}

}  // namespace fm1::platform

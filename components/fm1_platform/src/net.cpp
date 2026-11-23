#include "fm1/platform/net.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "fm1/package.hpp"
#include "fm1/platform/file.hpp"
#include "fm1/platform/library.hpp"
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
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(group, kWifiConnected | kIp4);
    net->note_disconnected();
    if (net->wifi_configured() && !net->wifi_reconfiguring()) esp_wifi_connect();
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
  std::array<uint8_t, 1024> buffer{};
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


void set_cors(httpd_req_t* request) {
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_set_hdr(request, "Access-Control-Max-Age", "600");
}

esp_err_t status_handler(httpd_req_t* request) {
  auto* net = static_cast<Net*>(request->user_ctx);
  std::string body = net != nullptr ? net->status_json() : std::string("{}");
  set_cors(request);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t options_handler(httpd_req_t* request) {
  set_cors(request);
  httpd_resp_set_status(request, "204 No Content");
  return httpd_resp_send(request, nullptr, 0);
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 4);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string read_meta_sha(const std::string& path) {
  std::FILE* file = std::fopen((path + ".meta").c_str(), "rb");
  if (file == nullptr) return {};
  char url[512]{};
  char sha[128]{};
  const bool ok = std::fgets(url, sizeof(url), file) != nullptr &&
                  std::fgets(sha, sizeof(sha), file) != nullptr;
  std::fclose(file);
  if (!ok) return {};
  sha[std::strcspn(sha, "\r\n")] = '\0';
  return std::strlen(sha) == 64 ? std::string(sha) : std::string{};
}

std::string url_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() &&
        std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      const auto value = [](char c) {
        return static_cast<unsigned>(c >= '0' && c <= '9' ? c - '0'
                                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                          : c - 'A' + 10);
      };
      out.push_back(static_cast<char>((value(text[i + 1]) << 4) | value(text[i + 2])));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

std::string sanitize_firmware_name(std::string name) {
  const auto slash = name.find_last_of("/\\");
  if (slash != std::string::npos) name.erase(0, slash + 1);
  if (!ends_with(name, ".fwsc")) return {};
  std::string out;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' ||
        c == '+') {
      out.push_back(c);
    }
  }
  if (!ends_with(out, ".fwsc") || out.size() > 120) return {};
  return out;
}

}  // namespace

void Net::set_status_provider(std::function<std::string()> provider) {
  status_provider_ = std::move(provider);
}

std::string Net::status_json() const {
  return status_provider_ ? status_provider_() : std::string("{}");
}

Result<void> Net::start(Storage& storage, const Config& config) {
  storage_ = &storage;
  catalog_url_ = config.catalog_url;
  set_device_name(config.device_name);
  ssid_ = config.ssid;
  configured_.store(!ssid_.empty());
  if (auto ready = ensure_wifi_driver(); !ready) {
    state_.store(WifiState::Failed);
    return ready;
  }
  if (ssid_.empty()) {
    state_.store(WifiState::Unconfigured);
    ESP_LOGW(kTag, "no Wi-Fi SSID configured; SD-only until set in SYSTEM");
    return {};
  }
  return apply_wifi(config);
}

Result<void> Net::ensure_wifi_driver() {
  if (driver_ready_) return {};
  if (event_group_ == nullptr) {
    event_group_ = xEventGroupCreate();
    if (event_group_ == nullptr) return fail("no Wi-Fi event group");
  }
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) return fail("NVS init failed");
  err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return fail("netif init failed");
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return fail("event loop failed");
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&init);
  if (err != ESP_OK) return fail("Wi-Fi init failed");
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, this,
                                      nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, this,
                                      nullptr);
  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return fail("Wi-Fi mode failed");
  err = esp_wifi_start();
  if (err != ESP_OK) return fail("Wi-Fi start failed");
  esp_wifi_set_ps(WIFI_PS_NONE);
  driver_ready_ = true;
  started_ = true;
  return {};
}

Result<void> Net::apply_wifi(const Config& config) {
  ssid_ = config.ssid;
  configured_.store(!ssid_.empty());
  if (auto ready = ensure_wifi_driver(); !ready) {
    state_.store(WifiState::Failed);
    return ready;
  }
  if (ssid_.empty()) {
    reconfiguring_.store(true);
    (void)esp_wifi_disconnect();
    reconfiguring_.store(false);
    state_.store(WifiState::Unconfigured);
    return {};
  }
  wifi_config_t wifi{};
  std::strncpy(reinterpret_cast<char*>(wifi.sta.ssid), ssid_.c_str(),
               sizeof(wifi.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi.sta.password), config.password.c_str(),
               sizeof(wifi.sta.password) - 1);
  wifi.sta.threshold.authmode =
      config.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  reconfiguring_.store(true);
  (void)esp_wifi_disconnect();
  const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
  reconfiguring_.store(false);
  if (err != ESP_OK) {
    state_.store(WifiState::Failed);
    return fail("Wi-Fi config failed");
  }
  state_.store(WifiState::Connecting);
  auto connected = connect_wifi();
  if (!connected) {
    state_.store(WifiState::Failed);
    std::lock_guard lock(error_lock_);
    wifi_error_ = connected.error().message;
    return connected;
  }
  state_.store(WifiState::Connected);
  auto services = start_http();
  if (!services) ESP_LOGW(kTag, "HTTP: %s", services.error().message.c_str());
  return {};
}

Result<std::vector<WifiNetwork>> Net::scan_networks() {
  if (auto ready = ensure_wifi_driver(); !ready) return fail(ready.error().message);
  wifi_scan_config_t scan{};
  scan.show_hidden = false;
  const esp_err_t err = esp_wifi_scan_start(&scan, true);
  if (err != ESP_OK) return fail("Wi-Fi scan failed");
  uint16_t count = 0;
  if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) return fail("Wi-Fi scan empty");
  count = std::min<uint16_t>(count, 32);
  std::vector<wifi_ap_record_t> records(count);
  if (count > 0 && esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
    return fail("Wi-Fi scan read failed");
  }
  records.resize(count);
  std::vector<WifiNetwork> networks;
  for (const auto& record : records) {
    const auto length = strnlen(reinterpret_cast<const char*>(record.ssid),
                                sizeof(record.ssid));
    if (length == 0) continue;
    const std::string name(reinterpret_cast<const char*>(record.ssid), length);
    const bool known = std::any_of(networks.begin(), networks.end(),
                                   [&](const WifiNetwork& network) {
                                     return network.ssid == name;
                                   });
    if (known) continue;
    networks.push_back({name, record.rssi, record.authmode != WIFI_AUTH_OPEN});
  }
  std::sort(networks.begin(), networks.end(),
            [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });
  return networks;
}

std::string Net::wifi_ssid() const {
  wifi_ap_record_t ap{};
  if (wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    const auto length = strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ap.ssid));
    return std::string(reinterpret_cast<const char*>(ap.ssid), length);
  }
  return ssid_;
}

std::string Net::local_ip() const {
  if (auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
      char ip[16]{};
      esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
      return ip;
    }
  }
  return {};
}

int Net::wifi_rssi() const {
  wifi_ap_record_t ap{};
  if (wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
  return -127;
}

esp_err_t Net::firmware_info(httpd_req_t* request) {
  if (storage_ == nullptr || !storage_->mounted()) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    return ESP_FAIL;
  }
  const std::size_t query_len = httpd_req_get_url_query_len(request);
  std::string name;
  if (query_len > 0 && query_len < 512) {
    std::string query(query_len + 1, '\0');
    if (httpd_req_get_url_query_str(request, query.data(), query.size()) == ESP_OK) {
      char value[160]{};
      if (httpd_query_key_value(query.c_str(), "name", value, sizeof(value)) == ESP_OK) {
        name = sanitize_firmware_name(url_decode(value));
      }
    }
  }
  if (name.empty()) {
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "name=*.fwsc required");
    return ESP_FAIL;
  }
  std::string path;
  std::string dir;
  for (const auto& candidate : {"fm1-card/firmwares", "firmwares"}) {
    const std::string candidate_path =
        storage_->path(std::string(candidate) + "/" + name);
    struct stat info {};
    if (stat(candidate_path.c_str(), &info) == 0 && info.st_size > 0) {
      path = candidate_path;
      dir = candidate;
      break;
    }
  }
  if (path.empty()) {
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "firmware not found");
    return ESP_FAIL;
  }
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "stat failed");
    return ESP_FAIL;
  }
  auto digest = sha256_file(path, nullptr);
  FileReader reader;
  std::string product;
  if (reader.open(path)) {
    auto parsed = package::inspect(reader);
    if (parsed) product = parsed->product;
  }
  const auto device = catalog::device_from_product(product);
  const auto variant = catalog::smk_variant_from_product(product);
  const char* variant_name = variant == catalog::SmkVariant::Elite   ? "elite"
                             : variant == catalog::SmkVariant::Mke   ? "mke"
                             : variant == catalog::SmkVariant::Pro   ? "pro"
                                                                    : "all";
  std::string body;
  body.reserve(400);
  body = "{\"name\":\"" + json_escape(name) + "\",\"dir\":\"" + json_escape(dir) +
         "\",\"size\":" + std::to_string(info.st_size) + ",\"sha256\":";
  body += digest ? "\"" + *digest + "\"" : "null";
  body += ",\"product\":\"" + json_escape(product) + "\",\"device\":\"" +
          (device == catalog::Device::Smk37 ? "smk37" : "fm1") +
          "\",\"variant\":\"" + variant_name + "\"}";
  set_cors(request);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t Net::list_firmwares(httpd_req_t* request) {
  if (storage_ == nullptr || !storage_->mounted()) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    return ESP_FAIL;
  }
  bool detail = false;
  const std::size_t query_len = httpd_req_get_url_query_len(request);
  if (query_len > 0 && query_len < 256) {
    std::string query(query_len + 1, '\0');
    if (httpd_req_get_url_query_str(request, query.data(), query.size()) == ESP_OK) {
      char value[8]{};
      if (httpd_query_key_value(query.c_str(), "detail", value, sizeof(value)) == ESP_OK) {
        detail = std::string_view(value) == "1";
      }
    }
  }
  set_cors(request);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_send_chunk(request, "[", 1);
  bool first = true;
  std::string out;
  out.reserve(4096);
  const auto flush = [&]() {
    if (!out.empty()) {
      httpd_resp_send_chunk(request, out.data(), out.size());
      out.clear();
    }
  };
  for (const auto& dir : {"fm1-card/firmwares", "firmwares"}) {
    const std::string base = storage_->path(dir);
    DIR* handle = opendir(base.c_str());
    if (handle == nullptr) continue;
    while (dirent* item = readdir(handle)) {
      const std::string name = item->d_name;
      if (!ends_with(name, ".fwsc")) continue;
      const std::string path = base + "/" + name;
      std::string record;
      record.reserve(256);
      if (!first) record += ",";
      first = false;
      record += "{\"name\":\"" + json_escape(name) + "\",\"dir\":\"" +
                json_escape(dir) + "\"";
      if (detail) {
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) continue;
        const std::string sha = read_meta_sha(path);
        record += ",\"size\":" + std::to_string(info.st_size) + ",\"sha256\":";
        if (sha.empty()) record += "null";
        else record += "\"" + sha + "\"";
      }
      record += "}";
      out += record;
      if (out.size() >= 2048) flush();
    }
    closedir(handle);
  }
  flush();
  httpd_resp_send_chunk(request, "]", 1);
  httpd_resp_send_chunk(request, nullptr, 0);
  return ESP_OK;
}

esp_err_t Net::upload_firmware(httpd_req_t* request) {
  const auto log = [&](std::string_view text) {
    if (storage_ != nullptr && storage_->mounted()) {
      (void)storage_->write_text("fm1-card/logs/net.log", text);
    }
  };
  log("upload start\n");
  if (storage_ == nullptr || !storage_->mounted()) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    return ESP_FAIL;
  }
  const std::size_t query_len = httpd_req_get_url_query_len(request);
  std::string name;
  if (query_len > 0 && query_len < 512) {
    std::string query(query_len + 1, '\0');
    if (httpd_req_get_url_query_str(request, query.data(), query.size()) == ESP_OK) {
      char value[160]{};
      if (httpd_query_key_value(query.c_str(), "name", value, sizeof(value)) == ESP_OK) {
        name = sanitize_firmware_name(url_decode(value));
      }
    }
  }
  if (name.empty()) {
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "name=*.fwsc required");
    return ESP_FAIL;
  }
  if (request->content_len == 0 || request->content_len > 8 * 1024 * 1024) {
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "bad content length");
    return ESP_FAIL;
  }
  const std::string final_path = storage_->path(std::string("fm1-card/firmwares/") + name);
  const std::string temp_path = final_path + ".upload";
  std::FILE* file = std::fopen(temp_path.c_str(), "wb");
  if (file == nullptr) {
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
    return ESP_FAIL;
  }
  std::size_t remaining = request->content_len;
  std::array<char, 1024> buffer{};
  int timeouts = 0;
  while (remaining > 0) {
    const int got = httpd_req_recv(request, buffer.data(),
                                   std::min<std::size_t>(remaining, buffer.size()));
    if (got == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts > 10) {
        log("upload timeout\n");
        std::fclose(file);
        std::remove(temp_path.c_str());
        set_cors(request);
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "upload timed out");
        return ESP_FAIL;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    timeouts = 0;
    if (got <= 0) {
      std::fclose(file);
      std::remove(temp_path.c_str());
      set_cors(request);
      httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "upload interrupted");
      return ESP_FAIL;
    }
    if (std::fwrite(buffer.data(), 1, static_cast<std::size_t>(got), file) !=
        static_cast<std::size_t>(got)) {
      std::fclose(file);
      std::remove(temp_path.c_str());
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "short write");
      return ESP_FAIL;
    }
    remaining -= static_cast<std::size_t>(got);
  }
  std::fclose(file);
  log("upload received\n");

  FileReader reader;
  const auto opened = reader.open(temp_path);
  const auto layout = opened ? package::inspect(reader) : Result<package::Layout>(fail("open"));
  if (!opened || !layout) {
    std::remove(temp_path.c_str());
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "not a valid .fwsc");
    return ESP_FAIL;
  }
  log("upload validated\n");
  std::remove(final_path.c_str());
  if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(temp_path.c_str());
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot save file");
    return ESP_FAIL;
  }
  std::size_t written = 0;
  std::string digest;
  if (auto sha = sha256_file(final_path, &written); sha) {
    digest = *sha;
    if (std::FILE* meta = std::fopen((final_path + ".meta").c_str(), "wb")) {
      std::fprintf(meta, "local:%s\n%s\n%zu\n", name.c_str(), digest.c_str(), written);
      std::fclose(meta);
    }
  }
  log("upload saved\n");
  note_library_change();
  set_cors(request);
  httpd_resp_set_type(request, "application/json");
  std::string body = "{\"ok\":true,\"name\":\"" + json_escape(name) +
                     "\",\"size\":" + std::to_string(written) +
                     ",\"sha256\":" + (digest.empty() ? "null" : "\"" + digest + "\"") +
                     ",\"product\":\"" + json_escape(layout->product) + "\"}";
  return httpd_resp_send(request, body.data(), body.size());
}

esp_err_t Net::upload_catalog(httpd_req_t* request) {
  if (storage_ == nullptr || !storage_->mounted()) {
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD card");
    return ESP_FAIL;
  }
  if (request->content_len == 0 || request->content_len > 4 * 1024 * 1024) {
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "bad content length");
    return ESP_FAIL;
  }
  (void)storage_->ensure_dir("fm1-card");
  const std::string temp_rel = "fm1-card/firmwares.json.upload";
  const std::string temp_path = storage_->path(temp_rel);
  const std::string final_path = storage_->path(Library::manifest_path());
  std::FILE* file = std::fopen(temp_path.c_str(), "wb");
  if (file == nullptr) {
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
    return ESP_FAIL;
  }
  std::size_t remaining = request->content_len;
  std::array<char, 2048> buffer{};
  int timeouts = 0;
  while (remaining > 0) {
    const int got = httpd_req_recv(request, buffer.data(),
                                   std::min<std::size_t>(remaining, buffer.size()));
    if (got == HTTPD_SOCK_ERR_TIMEOUT) {
      if (++timeouts > 10) {
        std::fclose(file);
        std::remove(temp_path.c_str());
        set_cors(request);
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "upload timed out");
        return ESP_FAIL;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    timeouts = 0;
    if (got <= 0) {
      std::fclose(file);
      std::remove(temp_path.c_str());
      set_cors(request);
      httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "upload interrupted");
      return ESP_FAIL;
    }
    if (std::fwrite(buffer.data(), 1, static_cast<std::size_t>(got), file) !=
        static_cast<std::size_t>(got)) {
      std::fclose(file);
      std::remove(temp_path.c_str());
      set_cors(request);
      httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "short write");
      return ESP_FAIL;
    }
    remaining -= static_cast<std::size_t>(got);
  }
  std::fclose(file);

  auto entries = build_catalog_cache(*storage_, temp_rel);
  if (!entries) {
    std::remove(temp_path.c_str());
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, entries.error().message.c_str());
    return ESP_FAIL;
  }
  std::remove(final_path.c_str());
  if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(temp_path.c_str());
    set_cors(request);
    httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot save manifest");
    return ESP_FAIL;
  }
  note_library_change();
  set_cors(request);
  httpd_resp_set_type(request, "application/json");
  const std::string body = "{\"ok\":true,\"entries\":" + std::to_string(*entries) + "}";
  return httpd_resp_send(request, body.data(), body.size());
}

Result<std::size_t> Net::load_local_catalog() {
  if (storage_ == nullptr || !storage_->mounted()) return fail("no SD card");
  for (const auto& path : {Library::manifest_path(), std::string("firmwares.json")}) {
    struct stat info {};
    if (stat(storage_->path(path).c_str(), &info) == 0 && info.st_size > 0) {
      return build_catalog_cache(*storage_, path);
    }
  }
  return fail("no local manifest; upload fm1-card/firmwares.json");
}

Result<void> Net::start_http() {
  if (http_ != nullptr) return {};
  const auto log = [&](std::string_view text) {
    if (storage_ != nullptr && storage_->mounted()) {
      (void)storage_->write_text("fm1-card/logs/net.log", text);
    }
  };
  start_discovery();
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 10;
  config.max_open_sockets = 4;
  config.stack_size = 20480;
  config.lru_purge_enable = true;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    log("httpd_start failed\n");
    return fail("HTTP server failed");
  }
  http_ = server;
  httpd_uri_t status{};
  status.uri = "/status";
  status.method = HTTP_GET;
  status.handler = status_handler;
  status.user_ctx = this;
  httpd_register_uri_handler(server, &status);
  httpd_uri_t root = status;
  root.uri = "/";
  httpd_register_uri_handler(server, &root);

  httpd_uri_t info = status;
  info.uri = "/firmwares/info";
  info.method = HTTP_GET;
  info.handler = [](httpd_req_t* request) {
    return static_cast<Net*>(request->user_ctx)->firmware_info(request);
  };
  httpd_register_uri_handler(server, &info);

  httpd_uri_t list = status;
  list.uri = "/firmwares";
  list.method = HTTP_GET;
  list.handler = [](httpd_req_t* request) {
    return static_cast<Net*>(request->user_ctx)->list_firmwares(request);
  };
  httpd_register_uri_handler(server, &list);

  httpd_uri_t upload = status;
  upload.uri = "/firmwares";
  upload.method = HTTP_POST;
  upload.handler = [](httpd_req_t* request) {
    return static_cast<Net*>(request->user_ctx)->upload_firmware(request);
  };
  httpd_register_uri_handler(server, &upload);

  httpd_uri_t options{};
  options.uri = "/firmwares";
  options.method = HTTP_OPTIONS;
  options.handler = options_handler;
  httpd_register_uri_handler(server, &options);

  httpd_uri_t catalog = status;
  catalog.uri = "/catalog";
  catalog.method = HTTP_POST;
  catalog.handler = [](httpd_req_t* request) {
    return static_cast<Net*>(request->user_ctx)->upload_catalog(request);
  };
  httpd_register_uri_handler(server, &catalog);

  httpd_uri_t catalog_options{};
  catalog_options.uri = "/catalog";
  catalog_options.method = HTTP_OPTIONS;
  catalog_options.handler = options_handler;
  httpd_register_uri_handler(server, &catalog_options);

  log("http/udp started on 80/8135\n");
  return {};
}

void Net::start_discovery() {
  if (discovery_socket_ >= 0) return;
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(8135);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
    close(fd);
    return;
  }
  discovery_socket_ = fd;
  discovery_running_.store(true);
  xTaskCreate(&Net::discovery_entry, "fm1-discover", 4096, this, 1, nullptr);
}

void Net::stop_discovery() {
  discovery_running_.store(false);
  if (discovery_socket_ >= 0) {
    close(discovery_socket_);
    discovery_socket_ = -1;
  }
}

void Net::discovery_entry(void* arg) {
  static_cast<Net*>(arg)->discovery_loop();
}

void Net::discovery_loop() {
  char buffer[32]{};
  while (discovery_running_.load()) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int got = recvfrom(discovery_socket_, buffer, sizeof(buffer) - 1, 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
    if (got > 0) {
      buffer[got] = '\0';
      if (std::strcmp(buffer, "hello") == 0 || std::strcmp(buffer, "FM1CARD?") == 0) {
        char ip[16]{};
        if (auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
          esp_netif_ip_info_t info{};
          if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            esp_ip4addr_ntoa(&info.ip, ip, sizeof(ip));
          }
        }
        std::string name;
        {
          std::lock_guard lock(name_lock_);
          name = device_name_;
        }
        char reply[160]{};
        const int length = std::snprintf(reply, sizeof(reply), "fm1-card;%s;80;%s\n", ip,
                                        name.c_str());
        sendto(discovery_socket_, reply, std::max(0, length), 0,
               reinterpret_cast<sockaddr*>(&from), from_len);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
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

void Net::set_device_name(std::string name) {
  std::string clean = normalize_device_name(name);
  for (char& c : clean) {
    if (c == ';' || c == '\r' || c == '\n') c = '-';
  }
  std::lock_guard lock(name_lock_);
  device_name_ = std::move(clean);
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
    return fail("Wi-Fi is not configured; open SYSTEM and press W");
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
  stop_discovery();
  if (started_) {
    esp_wifi_stop();
    esp_wifi_deinit();
    started_ = false;
    driver_ready_ = false;
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
  log("upload validated\n");
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

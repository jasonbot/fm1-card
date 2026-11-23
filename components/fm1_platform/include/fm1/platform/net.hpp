#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "fm1/catalog.hpp"
#include "fm1/platform/board.hpp"
#include "fm1/platform/config.hpp"
#include "fm1/result.hpp"

namespace fm1::platform {

struct NetProgress {
  std::size_t received = 0;
  std::size_t total = 0;
  std::string_view phase;
};

enum class WifiState : uint8_t { Unconfigured, Connecting, Connected, Failed };

class Net {
 public:
  using ProgressFn = std::function<void(const NetProgress&)>;

  Result<void> start(Storage& storage, const Config& config);
  [[nodiscard]] bool wifi_connected() const;
  [[nodiscard]] bool wifi_configured() const { return configured_.load(); }
  [[nodiscard]] WifiState wifi_state() const { return state_.load(); }
  [[nodiscard]] std::string wifi_error() const;
  void note_connected();
  void note_disconnected();
  void stop();

  Result<void> refresh_catalog(const ProgressFn& progress);
  Result<void> download(const catalog::Entry& entry, const std::string& destination,
                        const ProgressFn& progress);
  // True when destination and its .meta sidecar match this entry and the
  // local file still hashes to the recorded digest.
  Result<bool> has_current(const catalog::Entry& entry, const std::string& destination);

 private:
  Result<void> fetch(const std::string& url, const ProgressFn& progress,
                     const std::function<bool(std::span<const char>)>& sink);
  Result<void> connect_wifi();
  Result<void> ensure_connected();

 public:
  [[nodiscard]] void* event_group() const { return event_group_; }

 private:

  Storage* storage_ = nullptr;
  std::string catalog_url_;
  std::string ssid_;
  mutable std::atomic<WifiState> state_{WifiState::Unconfigured};
  mutable std::mutex error_lock_;
  std::string wifi_error_;
  void* event_group_ = nullptr;
  std::atomic<bool> configured_{false};
  bool started_ = false;
};

}  // namespace fm1::platform

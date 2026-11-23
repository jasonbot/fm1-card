#pragma once

#include <cstdint>
#include <functional>
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

class Net {
 public:
  using ProgressFn = std::function<void(const NetProgress&)>;

  Result<void> start(Storage& storage, const Config& config);
  [[nodiscard]] bool wifi_connected() const;
  void stop();

  Result<void> refresh_catalog(const ProgressFn& progress);
  Result<void> download(const catalog::Entry& entry, const std::string& destination,
                        const ProgressFn& progress);

 private:
  Result<void> fetch(const std::string& url, const ProgressFn& progress,
                     const std::function<bool(std::span<const char>)>& sink);
  Result<void> connect_wifi();

 public:
  [[nodiscard]] void* event_group() const { return event_group_; }

 private:

  Storage* storage_ = nullptr;
  std::string catalog_url_;
  void* event_group_ = nullptr;
  bool started_ = false;
};

}  // namespace fm1::platform

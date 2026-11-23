#pragma once

// SD-side library: the streamed catalog cache and local .fwsc files.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fm1/catalog.hpp"
#include "fm1/platform/board.hpp"
#include "fm1/result.hpp"

namespace fm1::platform {

struct LocalPackage {
  std::string path;
  std::string product;
  uint64_t size = 0;
  uint8_t blocks = 0;
};

class Library {
 public:
  explicit Library(Storage& storage) : storage_(storage) {}

  // Scans the cache for entries matching the filter, returning at most `limit`
  // entries starting at `offset`.
  Result<std::vector<catalog::Entry>> page(std::string_view filter, std::size_t offset,
                                           std::size_t limit);
  Result<std::size_t> count();
  Result<std::optional<catalog::Entry>> find(std::string_view id);
  Result<std::vector<LocalPackage>> local_packages();
  Result<std::string> package_path(std::string_view id, std::string_view version);

  [[nodiscard]] static std::string cache_path() { return "fm1-card/catalog.bin"; }
  [[nodiscard]] static std::string firmwares_dir() { return "fm1-card/firmwares"; }

 private:
  Storage& storage_;
};

}  // namespace fm1::platform

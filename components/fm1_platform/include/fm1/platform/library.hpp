#pragma once

// SD-side library: the streamed catalog cache and local .fwsc files.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fm1/catalog.hpp"
#include "fm1/platform/board.hpp"
#include "fm1/result.hpp"

namespace fm1::platform {

struct LocalMetadata {
  std::string id;
  std::string name;
  std::string version;
  std::string author;
  std::string what;
  std::string tags;
  bool present = false;
};

struct LocalPackage {
  std::string path;
  std::string name;     // friendly catalog name, or filename stem
  std::string product;
  catalog::Device device = catalog::Device::Fm1;
  catalog::SmkVariant variant = catalog::SmkVariant::All;
  uint64_t size = 0;
  uint8_t blocks = 0;
  LocalMetadata metadata;
};

LocalMetadata read_local_metadata(const std::string& package_path);
Result<void> write_local_metadata(const std::string& package_path, std::string_view source,
                                  std::string_view sha256, std::size_t size,
                                  const catalog::Entry* entry = nullptr);


enum class ChipFilter : uint8_t { All, New, Old };

struct LibraryFilter {
  std::string text;
  std::optional<catalog::Device> device;
  ChipFilter chip = ChipFilter::All;
  catalog::SmkVariant variant = catalog::SmkVariant::All;
};

class Library {
 public:
  explicit Library(Storage& storage) : storage_(storage) {}

  // Scans the cache for entries matching the filter, returning at most `limit`
  // entries starting at `offset`.
  Result<std::vector<catalog::Entry>> page(const LibraryFilter& filter, std::size_t offset,
                                           std::size_t limit);
  Result<std::size_t> count(const LibraryFilter& filter);
  // Calls fn for every cached entry; stop early if fn returns false.
  Result<void> for_each(const std::function<bool(const catalog::Entry&)>& fn);
  Result<std::optional<catalog::Entry>> find(std::string_view id);
  // Inspects at most `limit` files; `total` receives the full count.
  Result<std::vector<LocalPackage>> local_packages(std::size_t limit = 64,
                                                   std::size_t* total = nullptr);
  // Sorted window of local packages; only `[offset, offset+limit)` is inspected.
  Result<std::vector<LocalPackage>> local_packages_page(
      std::size_t offset, std::size_t limit, std::size_t* total = nullptr,
      std::string_view text = {});
  Result<std::size_t> local_count();
  Result<std::vector<LocalMetadata>> local_metadata();
  Result<std::string> package_path(std::string_view id, std::string_view version);

  [[nodiscard]] static std::string cache_path() { return "fm1-card/catalog.bin"; }
  [[nodiscard]] static std::string firmwares_dir() { return "fm1-card/firmwares"; }
  [[nodiscard]] static std::string manifest_path() { return "fm1-card/firmwares.json"; }

 private:
  Storage& storage_;
};

// Parses a catalog JSON manifest from the SD card and atomically replaces
// catalog.bin. Accepts a bare array or {"firmwares":[...]}.
Result<std::size_t> build_catalog_cache(Storage& storage, std::string_view json_relative);

}  // namespace fm1::platform

#pragma once

// The `.fwsc` package layout: 0x30-byte blocks whose last byte is an identity
// marker, followed by the logical image the device pulls. The image is served
// straight out of the file; no temporary 1 MiB copy.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fm1/result.hpp"

namespace fm1::package {

constexpr std::size_t kBlock = 0x30;
constexpr std::size_t kKeep = 0x2F;
constexpr std::size_t kDefaultBlocks = 20;
constexpr std::size_t kMaxBlocks = 64;

struct Layout {
  std::size_t blocks = 0;
  std::string product;
  uint64_t file_size = 0;

  [[nodiscard]] uint64_t logical_size() const {
    return file_size - static_cast<uint64_t>(blocks);
  }
};

// Random-access byte source; implemented by the SD file adapter and tests.
class Reader {
 public:
  virtual ~Reader() = default;
  [[nodiscard]] virtual uint64_t size() const = 0;
  // Reads exactly dst.size() bytes or returns an error.
  virtual Result<std::size_t> read_at(uint64_t offset, std::span<uint8_t> dst) = 0;
};

// Parse the interleaved identity markers from the first `bytes`.
Result<Layout> inspect(std::span<const uint8_t> bytes);

// Inspect a file/reader without loading it.
Result<Layout> inspect(Reader& reader);

// The image with the marker bytes removed (tests, small packages).
std::vector<uint8_t> logical_image(std::span<const uint8_t> fwsc);

// Reads a range of the logical image from the package file.
class LogicalImage {
 public:
  LogicalImage(Reader& reader, Layout layout) : reader_(reader), layout_(layout) {}

  [[nodiscard]] const Layout& layout() const {
    return layout_;
  }
  [[nodiscard]] uint64_t size() const {
    return layout_.logical_size();
  }

  // Reads exactly dst.size() logical bytes or returns an error.
  Result<std::size_t> read(uint64_t logical_offset, std::span<uint8_t> dst);

 private:
  Reader& reader_;
  Layout layout_;
};

}  // namespace fm1::package

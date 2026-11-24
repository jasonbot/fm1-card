#include "fm1/package.hpp"

#include <algorithm>
#include <array>

namespace fm1::package {
namespace {

std::size_t block_count(std::span<const uint8_t> bytes) {
  std::size_t blocks = kDefaultBlocks;
  while (blocks < kMaxBlocks && blocks * kBlock + kKeep < bytes.size() &&
         bytes[blocks * kBlock + kKeep] == 0x7D) {
    ++blocks;
  }
  return blocks;
}

}  // namespace

Result<Layout> inspect(std::span<const uint8_t> bytes) {
  if (bytes.size() < kDefaultBlocks * kBlock) {
    return fail("not an FM-1 package: too short");
  }
  const std::size_t blocks = block_count(bytes);
  std::string product;
  for (std::size_t i = 0; i < blocks; ++i) {
    const uint8_t marker = bytes[i * kBlock + kKeep];
    if (marker != 0x7D) {
      const auto value = static_cast<uint8_t>(marker - static_cast<uint8_t>(i) - 1);
      product.push_back(static_cast<char>(value));
    }
  }
  if (product.empty()) {
    return fail("not an FM-1 package: no product identity");
  }
  return Layout{.blocks = blocks, .product = std::move(product), .file_size = bytes.size()};
}

Result<Layout> inspect(Reader& reader) {
  const uint64_t size = reader.size();
  if (size < kDefaultBlocks * kBlock) {
    return fail("not an FM-1 package: too short");
  }
  std::array<uint8_t, kMaxBlocks * kBlock> head{};
  const std::size_t want = static_cast<std::size_t>(
      std::min<uint64_t>(head.size(), size));
  auto read = reader.read_at(0, std::span<uint8_t>(head.data(), want));
  if (!read) {
    return std::unexpected(read.error());
  }
  auto layout = inspect(std::span<const uint8_t>(head.data(), want));
  if (!layout) return layout;
  layout->file_size = size;
  return layout;
}

std::vector<uint8_t> logical_image(std::span<const uint8_t> fwsc) {
  if (fwsc.size() < kDefaultBlocks * kBlock) {
    return {};
  }
  const std::size_t blocks = block_count(fwsc);
  std::vector<uint8_t> out;
  out.reserve(fwsc.size() - blocks);
  for (std::size_t i = 0; i < blocks; ++i) {
    out.insert(out.end(), fwsc.begin() + static_cast<std::ptrdiff_t>(i * kBlock),
               fwsc.begin() + static_cast<std::ptrdiff_t>(i * kBlock + kKeep));
  }
  out.insert(out.end(), fwsc.begin() + static_cast<std::ptrdiff_t>(blocks * kBlock),
             fwsc.end());
  return out;
}

Result<std::size_t> LogicalImage::read(uint64_t logical_offset, std::span<uint8_t> dst) {
  if (logical_offset > size() || dst.size() > size() - logical_offset) {
    return fail("read past the end of the firmware image");
  }
  const uint64_t interleaved = static_cast<uint64_t>(layout_.blocks) * kKeep;
  std::size_t done = 0;
  while (done < dst.size()) {
    const uint64_t logical = logical_offset + done;
    uint64_t file_offset = 0;
    std::size_t run = 0;
    if (logical < interleaved) {
      const uint64_t block = logical / kKeep;
      const std::size_t within = static_cast<std::size_t>(logical % kKeep);
      file_offset = block * kBlock + within;
      run = std::min<std::size_t>(kKeep - within, dst.size() - done);
    } else {
      file_offset = logical + layout_.blocks;
      run = dst.size() - done;
    }
    auto result = reader_.read_at(file_offset, dst.subspan(done, run));
    if (!result) {
      return std::unexpected(result.error());
    }
    if (*result != run) {
      return fail("short read from the firmware package");
    }
    done += run;
  }
  return done;
}

}  // namespace fm1::package

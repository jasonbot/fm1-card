#include "fm1/platform/library.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

#include "fm1/package.hpp"
#include "fm1/platform/file.hpp"

namespace fm1::platform {
namespace {

Result<std::optional<std::vector<uint8_t>>> read_record(std::FILE* file) {
  uint8_t length_bytes[4]{};
  const auto got = std::fread(length_bytes, 1, 4, file);
  if (got == 0) return std::optional<std::vector<uint8_t>>{};
  if (got != 4) return fail("truncated catalog cache");
  const uint32_t length = static_cast<uint32_t>(length_bytes[0]) |
                          (static_cast<uint32_t>(length_bytes[1]) << 8) |
                          (static_cast<uint32_t>(length_bytes[2]) << 16) |
                          (static_cast<uint32_t>(length_bytes[3]) << 24);
  if (length == 0 || length > 8192) return fail("bad catalog record size");
  std::vector<uint8_t> bytes(length);
  if (std::fread(bytes.data(), 1, length, file) != length) {
    return fail("truncated catalog cache");
  }
  return bytes;
}

std::string sanitise(std::string_view text) {
  std::string out;
  for (char c : text) {
    out.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
                          c == '.'
                      ? c
                      : '_');
  }
  return out;
}

}  // namespace

Result<std::vector<catalog::Entry>> Library::page(const LibraryFilter& filter,
                                                  std::size_t offset, std::size_t limit) {
  std::vector<catalog::Entry> out;
  if (!storage_.mounted()) return out;
  std::FILE* file = std::fopen(storage_.path(cache_path()).c_str(), "rb");
  if (file == nullptr) return out;
  std::size_t matched = 0;
  while (out.size() < limit) {
    auto record = read_record(file);
    if (!record) {
      std::fclose(file);
      return std::unexpected(record.error());
    }
    if (!*record) break;
    auto entry = catalog::decode_record(**record);
    if (!entry) {
      std::fclose(file);
      return std::unexpected(entry.error());
    }
    if (filter.device && entry->device != *filter.device) continue;
    if (filter.chip == ChipFilter::New && !catalog::works_on_new_chip(*entry)) continue;
    if (filter.chip == ChipFilter::Old && !catalog::works_on_old_chip(*entry)) continue;
    if (!catalog::matches_smk_variant(*entry, filter.variant)) continue;
    if (!catalog::matches(*entry, filter.text)) continue;
    if (matched++ >= offset) out.push_back(std::move(*entry));
  }
  std::fclose(file);
  return out;
}

Result<std::size_t> Library::count(const LibraryFilter& filter) {
  if (!storage_.mounted()) return 0;
  std::FILE* file = std::fopen(storage_.path(cache_path()).c_str(), "rb");
  if (file == nullptr) return 0;
  std::size_t total = 0;
  for (;;) {
    auto record = read_record(file);
    if (!record) {
      std::fclose(file);
      return std::unexpected(record.error());
    }
    if (!*record) break;
    auto entry = catalog::decode_record(**record);
    if (!entry) {
      std::fclose(file);
      return std::unexpected(entry.error());
    }
    if (filter.device && entry->device != *filter.device) continue;
    if (filter.chip == ChipFilter::New && !catalog::works_on_new_chip(*entry)) continue;
    if (filter.chip == ChipFilter::Old && !catalog::works_on_old_chip(*entry)) continue;
    if (!catalog::matches_smk_variant(*entry, filter.variant)) continue;
    if (!catalog::matches(*entry, filter.text)) continue;
    ++total;
  }
  std::fclose(file);
  return total;
}

Result<std::optional<catalog::Entry>> Library::find(std::string_view id) {
  if (!storage_.mounted()) return std::optional<catalog::Entry>{};
  std::FILE* file = std::fopen(storage_.path(cache_path()).c_str(), "rb");
  if (file == nullptr) return std::optional<catalog::Entry>{};
  for (;;) {
    auto record = read_record(file);
    if (!record) {
      std::fclose(file);
      return std::unexpected(record.error());
    }
    if (!*record) break;
    auto entry = catalog::decode_record(**record);
    if (!entry) {
      std::fclose(file);
      return std::unexpected(entry.error());
    }
    if (entry->id == id) {
      std::fclose(file);
      return std::optional<catalog::Entry>(std::move(*entry));
    }
  }
  std::fclose(file);
  return std::optional<catalog::Entry>{};
}

Result<std::vector<LocalPackage>> Library::local_packages() {
  std::vector<LocalPackage> out;
  if (!storage_.mounted()) return out;
  const std::array<std::string, 2> dirs{"fm1-card/firmwares", "firmwares"};
  for (const auto& dir : dirs) {
    DIR* handle = opendir(storage_.path(dir).c_str());
    if (handle == nullptr) continue;
    while (dirent* item = readdir(handle)) {
      const std::string name = item->d_name;
      if (name.size() < 6 || name.substr(name.size() - 5) != ".fwsc") continue;
      const auto path = storage_.path(dir + "/" + name);
      FileReader reader;
      if (!reader.open(path)) continue;
      auto layout = package::inspect(reader);
      if (!layout) continue;
      std::string display = name.substr(0, name.size() - 5);
      for (char& c : display) {
        if (c == '_') c = ' ';
      }
      out.push_back(LocalPackage{.path = path,
                                 .name = std::move(display),
                                 .product = layout->product,
                                 .device = catalog::device_from_product(layout->product),
                                 .variant = catalog::smk_variant_from_product(layout->product),
                                 .size = layout->file_size,
                                 .blocks = static_cast<uint8_t>(layout->blocks)});
    }
    closedir(handle);
  }
  std::sort(out.begin(), out.end(),
            [](const LocalPackage& a, const LocalPackage& b) { return a.path < b.path; });
  return out;
}

Result<std::string> Library::package_path(std::string_view id, std::string_view version) {
  (void)storage_.ensure_dir(firmwares_dir());
  std::string name = sanitise(id);
  if (!version.empty()) name += "-" + sanitise(version);
  if (name.size() > 100) name.resize(100);
  name += ".fwsc";
  return storage_.path(firmwares_dir() + "/" + name);
}

}  // namespace fm1::platform

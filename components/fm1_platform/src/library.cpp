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

Result<void> Library::for_each(const std::function<bool(const catalog::Entry&)>& fn) {
  if (!storage_.mounted()) return fail("no SD card");
  std::FILE* file = std::fopen(storage_.path(cache_path()).c_str(), "rb");
  if (file == nullptr) return { };
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
    if (fn && !fn(*entry)) break;
  }
  std::fclose(file);
  return {};
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

Result<std::vector<LocalPackage>> Library::local_packages_page(
    std::size_t offset, std::size_t limit, std::size_t* total,
    std::string_view text) {
  std::vector<LocalPackage> out;
  if (!storage_.mounted()) return out;
  std::vector<std::string> paths;
  for (const auto& dir : std::array<std::string, 2>{"fm1-card/firmwares", "firmwares"}) {
    DIR* handle = opendir(storage_.path(dir).c_str());
    if (handle == nullptr) continue;
    while (dirent* item = readdir(handle)) {
      const std::string name = item->d_name;
      if (name.size() < 6 || name.substr(name.size() - 5) != ".fwsc") continue;
      paths.push_back(storage_.path(dir + "/" + name));
    }
    closedir(handle);
  }
  std::sort(paths.begin(), paths.end());
  if (!text.empty()) {
    std::erase_if(paths, [&](const std::string& path) {
      catalog::Entry entry;
      entry.name = path;
      return !catalog::matches(entry, text);
    });
  }
  if (total != nullptr) *total = paths.size();
  const std::size_t end = std::min(paths.size(), offset + limit);
  for (std::size_t i = offset; i < end; ++i) {
    FileReader reader;
    if (!reader.open(paths[i])) continue;
    auto layout = package::inspect(reader);
    if (!layout) continue;
    std::string display = paths[i].substr(paths[i].rfind('/') + 1);
    display.resize(display.size() - 5);
    for (char& c : display) {
      if (c == '_') c = ' ';
    }
    out.push_back(LocalPackage{.path = paths[i],
                               .name = std::move(display),
                               .product = layout->product,
                               .device = catalog::device_from_product(layout->product),
                               .variant = catalog::smk_variant_from_product(layout->product),
                               .size = layout->file_size,
                               .blocks = static_cast<uint8_t>(layout->blocks)});
  }
  return out;
}

Result<std::vector<LocalPackage>> Library::local_packages(std::size_t limit,
                                                          std::size_t* total) {
  return local_packages_page(0, limit, total, {});
}

Result<std::size_t> Library::local_count() {
  if (!storage_.mounted()) return 0;
  std::size_t found = 0;
  const std::array<std::string, 2> dirs{"fm1-card/firmwares", "firmwares"};
  for (const auto& dir : dirs) {
    DIR* handle = opendir(storage_.path(dir).c_str());
    if (handle == nullptr) continue;
    while (dirent* item = readdir(handle)) {
      const std::string name = item->d_name;
      if (name.size() >= 6 && name.substr(name.size() - 5) == ".fwsc") ++found;
    }
    closedir(handle);
  }
  return found;
}

Result<std::size_t> build_catalog_cache(Storage& storage, std::string_view json_relative) {
  if (!storage.mounted()) return fail("no SD card");
  const auto input_path = storage.path(json_relative);
  std::FILE* input = std::fopen(input_path.c_str(), "rb");
  if (input == nullptr) return fail("cannot open " + std::string(json_relative));

  const auto final_path = storage.path(Library::cache_path());
  const auto temp_path = final_path + ".tmp";
  std::FILE* output = std::fopen(temp_path.c_str(), "wb");
  if (output == nullptr) {
    std::fclose(input);
    return fail("cannot create catalog temp file");
  }

  std::size_t entries = 0;
  bool write_ok = true;
  catalog::StreamParser parser([&](catalog::Entry&& entry) {
    const auto record = catalog::encode_record(entry);
    const uint32_t length = static_cast<uint32_t>(record.size());
    const uint8_t header[4]{static_cast<uint8_t>(length),
                            static_cast<uint8_t>(length >> 8),
                            static_cast<uint8_t>(length >> 16),
                            static_cast<uint8_t>(length >> 24)};
    if (std::fwrite(header, 1, 4, output) != 4 ||
        std::fwrite(record.data(), 1, record.size(), output) != record.size()) {
      write_ok = false;
    }
    ++entries;
  });

  std::array<char, 4096> buffer{};
  while (const auto got = std::fread(buffer.data(), 1, buffer.size(), input)) {
    parser.feed(std::span<const char>(buffer.data(), got));
  }
  std::fclose(input);
  auto finish = parser.finish();
  std::fflush(output);
  std::fclose(output);

  if (!write_ok || !finish || entries == 0) {
    std::remove(temp_path.c_str());
    if (!finish) return fail(finish.error().message);
    if (entries == 0) return fail("manifest has no firmware entries");
    return fail("could not write the catalog cache");
  }
  std::remove(final_path.c_str());
  if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
    std::remove(temp_path.c_str());
    return fail("cannot replace the catalog cache");
  }
  return entries;
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

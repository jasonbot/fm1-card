#include "fm1/flash_analysis.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

#include "fm1/wl82.hpp"

namespace fm1::flash {
namespace {

constexpr std::size_t kConfigKey = 34;
constexpr std::size_t kConfigSize = kBootArea - kConfigAt;
constexpr std::array<uint8_t, kConfigSize - kConfigKey> kConfigSettings{
    0x04, 'O',  'S',  'C',  0x00, 'O',  'S',  'C',  '0', 0x04, 'O',  'S',  'C',  '_',  'F',  'R',
    'E',  'Q',  0x00, 0x00, 0x36, 0x6E, 0x01, 0x04, 'S', 'Y',  'S',  '_',  'C',  'L',  'K',  0x00,
    0x00, 0x2A, 0x75, 0x15, 0x04, 'H',  'S',  'B',  '_', 'D',  'I',  'V',  0x00, 0x01, 0x00, 0x00,
    0x00, 0x04, 'L',  'S',  'B',  '_',  'D',  'I',  'V', 0x00, 0x02, 0x00, 0x00, 0x00,
};

uint16_t u16le(std::span<const uint8_t> bytes, std::size_t at) {
  return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}
uint32_t u32le(std::span<const uint8_t> bytes, std::size_t at) {
  return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
         (static_cast<uint32_t>(bytes[at + 2]) << 16) |
         (static_cast<uint32_t>(bytes[at + 3]) << 24);
}
void put16(std::span<uint8_t> bytes, std::size_t at, uint16_t value) {
  bytes[at] = static_cast<uint8_t>(value);
  bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}
void put32(std::span<uint8_t> bytes, std::size_t at, uint32_t value) {
  bytes[at] = static_cast<uint8_t>(value);
  bytes[at + 1] = static_cast<uint8_t>(value >> 8);
  bytes[at + 2] = static_cast<uint8_t>(value >> 16);
  bytes[at + 3] = static_cast<uint8_t>(value >> 24);
}

std::string cstr(std::span<const uint8_t> bytes) {
  std::string out;
  for (uint8_t byte : bytes) {
    if (byte == 0) break;
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

std::vector<uint8_t> decrypt(std::span<const uint8_t> bytes, uint16_t key) {
  std::vector<uint8_t> out;
  out.reserve(bytes.size());
  for (uint8_t byte : bytes) {
    const uint8_t plain = static_cast<uint8_t>(byte ^ (key & 0xFF));
    key = static_cast<uint16_t>((key << 1) ^ ((key & 0x8000) != 0 ? 0x1021 : 0));
    out.push_back(plain);
  }
  return out;
}

std::vector<uint8_t> encrypt(std::span<const uint8_t> bytes, uint16_t key) {
  return decrypt(bytes, key);
}

struct Entry {
  std::array<uint8_t, 32> raw{};
  uint16_t data_crc = 0;
  uint32_t address = 0;
  uint32_t length = 0;
  std::string name;
  bool valid = false;
};

Entry entry(std::span<const uint8_t> raw) {
  Entry result;
  if (raw.size() < 32) return result;
  std::copy_n(raw.begin(), 32, result.raw.begin());
  result.data_crc = u16le(result.raw, 2);
  result.address = u32le(result.raw, 4);
  result.length = u32le(result.raw, 8);
  result.name = cstr(std::span<const uint8_t>(result.raw).subspan(16, 16));
  result.valid =
      wl82::crc16(std::span<const uint8_t>(result.raw).subspan(2)) == u16le(result.raw, 0);
  return result;
}

bool header_ok(std::span<const uint8_t> flash) {
  if (flash.size() < 32) return false;
  const auto head = decrypt(flash.subspan(0, 32), 0xFFFF);
  return wl82::crc16(std::span<const uint8_t>(head).subspan(2)) == u16le(head, 0);
}

std::vector<Entry> boot_table(std::span<const uint8_t> flash) {
  std::vector<Entry> entries;
  if (flash.size() < kBootArea || !header_ok(flash)) return entries;
  for (std::size_t at = 0x20; at < kBootAt; at += 0x20) {
    const auto decrypted = decrypt(flash.subspan(at, 32), 0xFFFF);
    auto parsed = entry(decrypted);
    if (!parsed.valid) break;
    entries.push_back(std::move(parsed));
  }
  return entries;
}

std::vector<uint8_t> app_plain(std::span<const uint8_t> flash, std::size_t offset,
                               std::size_t length, uint16_t key) {
  std::vector<uint8_t> output;
  output.reserve(length + 64);
  std::size_t at = offset - offset % 32;
  while (at < offset + length) {
    const std::size_t start = kBootArea + at;
    if (start + 32 > flash.size()) break;
    const auto block = decrypt(flash.subspan(start, 32),
                               static_cast<uint16_t>(key ^ static_cast<uint16_t>(at >> 2)));
    output.insert(output.end(), block.begin(), block.end());
    at += 32;
  }
  const std::size_t skip = offset % 32;
  if (skip >= output.size()) return {};
  output.erase(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(skip));
  if (output.size() > length) output.resize(length);
  return output;
}

std::vector<Entry> app_table(std::span<const uint8_t> flash, uint16_t key) {
  std::vector<Entry> entries;
  for (std::size_t index = 0; index < 16; ++index) {
    const auto raw = app_plain(flash, index * 32, 32, key);
    if (raw.size() < 32) break;
    auto parsed = entry(raw);
    if (!parsed.valid) break;
    entries.push_back(std::move(parsed));
  }
  return entries;
}

std::optional<std::vector<uint8_t>> stored_bootloader(std::span<const uint8_t> flash) {
  const auto table = boot_table(flash);
  if (table.empty()) return std::nullopt;
  const auto& first = table.front();
  if (first.name != "uboot.boot" || first.address != kBootAt ||
      kBootAt + first.length > flash.size()) {
    return std::nullopt;
  }
  const auto stored = flash.subspan(kBootAt, first.length);
  auto boot = decrypt(stored.subspan(0, 16), 0xFFFF);
  const auto rest = decrypt(stored.subspan(16), 0xFFFF);
  boot.insert(boot.end(), rest.begin(), rest.end());
  if (wl82::crc16(boot) != first.data_crc) return std::nullopt;
  return boot;
}

bool contains(std::span<const uint8_t> haystack, std::span<const uint8_t> needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
         haystack.end();
}

std::vector<std::pair<std::string, std::string>> identity_texts(std::span<const uint8_t> bytes) {
  static constexpr std::array<std::string_view, 5> kModels{"FM-1", "MiniFM", "SMK-37 Pro",
                                                           "SMK-37 Elite", "SMK-37+VA"};
  std::vector<std::pair<std::string, std::string>> found;
  for (const auto model : kModels) {
    for (std::size_t at = 0; at + model.size() + 4 <= bytes.size(); ++at) {
      if (std::equal(model.begin(), model.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at)) &&
          bytes[at + model.size()] == '_') {
        const auto digits = bytes.subspan(at + model.size() + 1, 3);
        if (std::all_of(digits.begin(), digits.end(),
                        [](uint8_t byte) { return byte >= '0' && byte <= '9'; })) {
          found.push_back({std::string(model), std::string(digits.begin(), digits.end())});
        }
      }
    }
  }
  return found;
}

std::optional<std::string> after_version(std::span<const uint8_t> bytes, std::string_view prefix) {
  for (std::size_t at = 0; at + prefix.size() < bytes.size(); ++at) {
    if (std::equal(prefix.begin(), prefix.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at))) {
      const std::size_t start = at + prefix.size();
      std::size_t end = start;
      while (end < bytes.size() &&
             ((bytes[end] >= '0' && bytes[end] <= '9') || bytes[end] == '.')) {
        ++end;
      }
      if (end > start && bytes[start] >= '0' && bytes[start] <= '9') {
        std::string out(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                        bytes.begin() + static_cast<std::ptrdiff_t>(end));
        while (!out.empty() && out.back() == '.') out.pop_back();
        return out;
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::pair<std::string, std::string>> bootloader_identity(
    std::span<const uint8_t> boot) {
  if (boot.size() < 16) return std::nullopt;
  const uint16_t bank = u16le(boot, 0);
  const std::size_t size = u16le(boot, 2);
  const std::size_t offset = u32le(boot, 8);
  if (bank != 1 || wl82::crc16(boot.subspan(0, 14)) != u16le(boot, 14) ||
      offset + size > boot.size()) {
    return std::nullopt;
  }
  const auto payload = boot.subspan(offset, size);
  if (wl82::crc16(payload) != u16le(boot, 12) || payload.size() < 0x70 ||
      !std::equal(payload.begin() + 0x40, payload.begin() + 0x48, "AC79BLID")) {
    return std::nullopt;
  }
  return std::pair{cstr(payload.subspan(0x48, 8)), cstr(payload.subspan(0x50, 0x20))};
}

Analysis analyze(std::span<const uint8_t> flash, uint32_t flash_id, uint16_t chip_key) {
  Analysis result;
  const auto table = app_table(flash, chip_key);
  const auto boot = stored_bootloader(flash);
  const auto identity = boot ? bootloader_identity(*boot) : std::nullopt;

  if (identity && identity->first == "fm1") {
    result.board_key = "fm1";
  } else if (identity && identity->first == "smk37") {
    result.board_key = "smk37";
  } else if (flash_id == 0x856014 || flash_id == 0xB36014) {
    result.board_key = "fm1";
  } else if (flash_id == 0xEB6014) {
    result.board_key = "smk37";
  }
  result.model_known = !result.board_key.empty();
  result.model = result.board_key == "fm1"     ? "FM-1"
                 : result.board_key == "smk37" ? "SMK-37 Pro / Elite"
                                               : "unknown";
  result.bootloader = identity
                          ? "open bootloader " + identity->second + " (" + identity->first + ")"
                      : boot ? "stock"
                             : "unreadable";

  const auto app_entry = std::find_if(table.begin(), table.end(),
                                      [](const Entry& item) { return item.name == "app.bin"; });
  if (app_entry == table.end()) {
    result.app = "none installed";
    result.firmware = "none";
    return result;
  }
  const auto text = app_plain(flash, app_entry->address, app_entry->length, chip_key);
  result.app = wl82::crc16(text) == app_entry->data_crc ? "OK" : "checksum failed";
  const bool baud = std::any_of(table.begin(), table.end(), [](const Entry& item) {
    return item.name == "VM" && item.address == 0xD9000;
  });
  if (const auto sloop = after_version(text, "SLOOP ")) {
    result.firmware = "SLOOP " + *sloop;
  } else if (contains(text,
                      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("Felucca"), 7))) {
    result.firmware = "Felucca";
  } else {
    const auto names = identity_texts(text);
    if (names.empty()) {
      result.firmware = "unknown";
    } else {
      const auto& [name, version] = names.back();
      result.firmware = (baud || name == "SMK-37+VA") ? name + " " + version + " (Baud Girl)"
                                                      : name + " " + version;
    }
  }
  return result;
}

Result<void> build_boot_area(std::span<const uint8_t> flash, std::span<const uint8_t> bootloader,
                             std::span<uint8_t> area) {
  if (flash.size() < kBootArea) {
    return fail("the boot area is incomplete");
  }
  if (area.size() < kBootArea) {
    return fail("the output boot area is incomplete");
  }
  const auto identity = bootloader_identity(bootloader);
  if (!identity) {
    return fail("not a valid open bootloader");
  }
  if (bootloader.size() > kConfigAt - kBootAt) {
    return fail("the bootloader is too large for the boot area");
  }
  const auto table = boot_table(flash);
  if (table.size() < 3 || table[0].name != "uboot.boot" || table[0].address != kBootAt) {
    return fail("the boot area has no usable file table");
  }
  const std::size_t config_at = table[1].address;
  const std::size_t config_len = table[1].length;
  if (table[1].name != "isd_config.ini" || config_at < 0x38D0 || config_at > kBootArea ||
      config_len < kConfigKey || config_len > kBootArea - config_at) {
    return fail("the boot configuration is not safely contained");
  }
  for (std::size_t i = 2; i < table.size(); ++i) {
    if (table[i].address < kBootArea) {
      return fail("other boot-area data would be overwritten");
    }
  }
  const auto key = flash.subspan(config_at, kConfigKey);
  if (wl82::crc16(key.subspan(0, 32)) != u16le(key, 32)) {
    return fail("the unit's chip-key record is damaged");
  }

  std::fill_n(area.begin(), kBootArea, 0xFF);
  std::copy_n(flash.begin(), kBootAt, area.begin());

  auto entry0 = table[0].raw;
  put16(entry0, 2, wl82::crc16(bootloader));
  put32(entry0, 8, static_cast<uint32_t>(bootloader.size()));
  put16(entry0, 0, wl82::crc16(std::span<const uint8_t>(entry0).subspan(2)));
  const auto encrypted0 = encrypt(entry0, 0xFFFF);
  std::copy(encrypted0.begin(), encrypted0.end(), area.begin() + 0x20);

  std::vector<uint8_t> config(key.begin(), key.end());
  config.insert(config.end(), kConfigSettings.begin(), kConfigSettings.end());
  auto entry1 = table[1].raw;
  put16(entry1, 2, wl82::crc16(config));
  put32(entry1, 4, static_cast<uint32_t>(config_at));
  put32(entry1, 8, static_cast<uint32_t>(kConfigSize));
  put16(entry1, 0, wl82::crc16(std::span<const uint8_t>(entry1).subspan(2)));
  const auto encrypted1 = encrypt(entry1, 0xFFFF);
  std::copy(encrypted1.begin(), encrypted1.end(), area.begin() + 0x40);

  const auto boot_head = encrypt(bootloader.subspan(0, 16), 0xFFFF);
  std::copy(boot_head.begin(), boot_head.end(), area.begin() + kBootAt);
  uint16_t key_state = 0xFFFF;
  for (std::size_t i = 16; i < bootloader.size(); ++i) {
    area[kBootAt + i] = static_cast<uint8_t>(bootloader[i] ^ (key_state & 0xFF));
    key_state = static_cast<uint16_t>((key_state << 1) ^ ((key_state & 0x8000) != 0 ? 0x1021 : 0));
  }
  std::copy(config.begin(), config.end(), area.begin() + static_cast<std::ptrdiff_t>(config_at));
  return {};
}

Result<std::vector<uint8_t>> build_boot_area(std::span<const uint8_t> flash,
                                             std::span<const uint8_t> bootloader) {
  std::vector<uint8_t> area(kBootArea, 0xFF);
  auto built = build_boot_area(flash, bootloader, area);
  if (!built) return std::unexpected(built.error());
  return area;
}

}  // namespace fm1::flash

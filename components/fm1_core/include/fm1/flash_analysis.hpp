#pragma once

// Boot-area parsing for the FM-1/SMK-37 JieLi flash, and construction of a
// replacement boot area that keeps the unit's chip-key record. Ported from
// fm1-recovery; used before installing the open m-bootloader.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fm1/result.hpp"

namespace fm1::flash {

constexpr std::size_t kFlashSize = 0x100000;
constexpr std::size_t kBootArea = 0x4000;
constexpr std::size_t kBootAt = 0xA0;
constexpr std::size_t kConfigAt = 0x3FA0;

struct Analysis {
  std::string model;
  bool model_known = false;
  std::string firmware;
  std::string app;
  std::string bootloader;
  std::string board_key;  // "fm1", "smk37", or empty
};

Analysis analyze(std::span<const uint8_t> flash, uint32_t flash_id, uint16_t chip_key);

// The m-bootloader identity in a .boot file: ("fm1"|"smk37", version).
std::optional<std::pair<std::string, std::string>> bootloader_identity(
    std::span<const uint8_t> bootloader);

Result<std::vector<uint8_t>> build_boot_area(std::span<const uint8_t> flash,
                                             std::span<const uint8_t> bootloader);
// Same build, writing into caller storage instead of allocating 16 KiB.
Result<void> build_boot_area(std::span<const uint8_t> flash, std::span<const uint8_t> bootloader,
                             std::span<uint8_t> area);

}  // namespace fm1::flash

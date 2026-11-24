#pragma once

// JieLi (WL82) ROM USB-disk protocol used by the FM-1 download mode:
// vendor SCSI CDBs over the MSC bulk-only transport, plus the loader upload.
//
// Nothing here knows about USB. The platform supplies a ScsiTransport backed
// by TinyUSB's tuh_msc_scsi_command().

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fm1/result.hpp"

namespace fm1::wl82 {

constexpr std::size_t kFlashSize = 0x100000;
constexpr uint32_t kLoaderAddress = 0x01C2'0000;

// Vendor command words (first two CDB bytes, big endian).
constexpr uint16_t kWriteMemory = 0xFB06;
constexpr uint16_t kJump = 0xFB08;
constexpr uint16_t kEraseBlock = 0xFB00;
constexpr uint16_t kEraseSector = 0xFB01;
constexpr uint16_t kWriteFlash = 0xFB04;
constexpr uint16_t kReadFlash = 0xFD05;
constexpr uint16_t kReadKey = 0xFC09;
constexpr uint16_t kOnlineDevice = 0xFC0A;
constexpr uint16_t kIdentify = 0xFC40;
constexpr uint16_t kCrc32 = 0xFC41;
constexpr uint16_t kBufferSize = 0xFC14;

// The loader image is bundled by the app (data/mflr-loader-a.bin); the core
// only needs a span.
using LogFn = std::function<void(std::string_view)>;

class ScsiTransport {
 public:
  virtual ~ScsiTransport() = default;
  // Data-in command: fills dst, returns bytes transferred.
  virtual Result<std::size_t> in(std::span<const uint8_t> cdb, std::span<uint8_t> dst) = 0;
  // Data-out command.
  virtual Result<std::size_t> out(std::span<const uint8_t> cdb, std::span<const uint8_t> src) = 0;
  // No data stage (status only).
  virtual Result<std::size_t> none(std::span<const uint8_t> cdb) = 0;
};

uint16_t crc16(std::span<const uint8_t> bytes);
uint32_t crc32(std::span<const uint8_t> bytes);
std::array<uint8_t, 16> cdb(uint16_t command, std::span<const uint8_t> args);
std::vector<uint8_t> cipher(std::span<const uint8_t> bytes, uint32_t key);

// Inquiry strings (36-byte response).
Result<std::pair<std::string, std::string>> inquiry(ScsiTransport& transport);

class Loader {
 public:
  Loader(ScsiTransport& transport, LogFn log) : transport_(transport), log_(std::move(log)) {}

  // Verifies WL82/UBOOT1.00, uploads the loader if needed, reads chip key and
  // flash id. The loader image must live for the duration of open().
  Result<void> open(std::span<const uint8_t> loader_image);

  [[nodiscard]] uint16_t chip_key() const {
    return chip_key_;
  }
  [[nodiscard]] uint32_t flash_id() const {
    return flash_id_;
  }
  [[nodiscard]] std::size_t io_size() const {
    return io_;
  }

  Result<std::vector<uint8_t>> read(uint32_t address, std::size_t length);
  Result<void> write(uint32_t address, std::span<const uint8_t> bytes);
  Result<void> erase(uint32_t address, std::size_t length);
  Result<uint32_t> crc32_of(uint32_t address, std::size_t length);

 private:
  Result<std::array<uint8_t, 14>> call(uint16_t command, std::span<const uint8_t> args);
  Result<void> upload(uint32_t address, std::span<const uint8_t> image);
  Result<void> jump(uint32_t address);
  Result<void> identify();

  ScsiTransport& transport_;
  LogFn log_;
  uint16_t chip_key_ = 0;
  uint32_t flash_id_ = 0;
  std::size_t io_ = 4096;
};

}  // namespace fm1::wl82

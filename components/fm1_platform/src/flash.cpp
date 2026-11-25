#include "fm1/platform/flash.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fm1/flash_analysis.hpp"
#include "fm1/identity.hpp"
#include "fm1/midi.hpp"
#include "fm1/package.hpp"
#include "fm1/platform/file.hpp"
#include "fm1/update.hpp"
#include "fm1/wl82.hpp"
#include "psa/crypto.h"

extern "C" {
extern const uint8_t _binary_mflr_loader_a_bin_start[];
extern const uint8_t _binary_mflr_loader_a_bin_end[];
extern const uint8_t _binary_bootloader_fm1_boot_start[];
extern const uint8_t _binary_bootloader_fm1_boot_end[];
extern const uint8_t _binary_bootloader_smk37_boot_start[];
extern const uint8_t _binary_bootloader_smk37_boot_end[];
}

namespace fm1::platform {
namespace {

constexpr const char* kTag = "fm1-flash";

std::span<const uint8_t> bytes(const uint8_t* begin, const uint8_t* end) {
  return std::span<const uint8_t>(begin, static_cast<std::size_t>(end - begin));
}

uint32_t crc32_update(uint32_t crc, std::span<const uint8_t> bytes) {
  static std::array<uint32_t, 256> table{};
  static bool ready = false;
  if (!ready) {
    for (std::size_t i = 0; i < table.size(); ++i) {
      uint32_t value = static_cast<uint32_t>(i);
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1) != 0 ? 0xEDB8'8320u ^ (value >> 1) : value >> 1;
      }
      table[i] = value;
    }
    ready = true;
  }
  crc = ~crc;
  for (uint8_t byte : bytes) {
    crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

class MscTransport : public wl82::ScsiTransport {
 public:
  MscTransport(UsbHost& usb, uint8_t slot) : usb_(usb), slot_(slot) {}

  Result<std::size_t> in(std::span<const uint8_t> cdb, std::span<uint8_t> dst) override {
    msc_cbw_t cbw{};
    cbw.signature = 0x43425355;
    cbw.tag = slot_;
    cbw.dir = 1;
    cbw.cmd_len = 16;
    std::memcpy(cbw.command, cdb.data(), std::min<std::size_t>(cdb.size(), 16));
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (usb_.scsi_command(slot_, cbw, dst, {}, std::chrono::seconds(30))) {
        return dst.size();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
    }
    return fail("SCSI data-in command failed");
  }
  Result<std::size_t> out(std::span<const uint8_t> cdb, std::span<const uint8_t> src) override {
    msc_cbw_t cbw{};
    cbw.signature = 0x43425355;
    cbw.tag = slot_;
    cbw.dir = 0;
    cbw.cmd_len = 16;
    std::memcpy(cbw.command, cdb.data(), std::min<std::size_t>(cdb.size(), 16));
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (usb_.scsi_command(slot_, cbw, {}, src, std::chrono::seconds(30))) {
        return src.size();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
    }
    return fail("SCSI data-out command failed");
  }
  Result<std::size_t> none(std::span<const uint8_t> cdb) override {
    msc_cbw_t cbw{};
    cbw.signature = 0x43425355;
    cbw.tag = slot_;
    cbw.dir = 0;
    cbw.cmd_len = 16;
    std::memcpy(cbw.command, cdb.data(), std::min<std::size_t>(cdb.size(), 16));
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (usb_.scsi_command(slot_, cbw, {}, {}, std::chrono::seconds(30))) {
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
    }
    return fail("SCSI status command failed");
  }

 private:
  UsbHost& usb_;
  uint8_t slot_;
};

struct ClaimGuard {
  UsbHost& usb;
  uint8_t slot;
  ~ClaimGuard() {
    usb.claim(slot, false);
    usb.mute(slot, false);
  }
};

std::string timestamp() {
  std::time_t now = std::time(nullptr);
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", std::localtime(&now));
  return buffer;
}

std::string model_of(std::string_view identity) {
  return std::string(fm1::identity::model_of(identity));
}

bool is_ota(const MidiPort& port) {
  return port.identity.starts_with("ota-") || port.identity.starts_with("OTA-");
}

constexpr std::size_t kSector = 0x1000;
constexpr std::uint32_t kAppStart = 0x4000;
constexpr std::uint32_t kAppEnd = 0x93000;
constexpr std::uint16_t kFm1ChipKey = 0x980F;
constexpr std::uint32_t kFm1FlashId = 0x856014;
constexpr std::uint32_t kFm1FlashId2 = 0xB36014;
constexpr std::string_view kV15Hash =
    "6edf3c37fb5bbbc33607c89375ee024d5477c17914d72221c8c68e58a8255686";

std::string board_key_for(std::uint32_t flash_id) {
  if (flash_id == 0x856014 || flash_id == 0xB36014) return "fm1";
  if (flash_id == 0xEB6014) return "smk37";
  return {};
}

std::array<std::uint8_t, flash::kBootArea> g_boot_area{};
std::array<std::uint8_t, flash::kBootArea> g_boot_build{};

Result<void> read_boot_area(wl82::Loader& loader) {
  for (std::size_t at = 0; at < g_boot_area.size(); at += kSector) {
    auto chunk =
        loader.read(static_cast<std::uint32_t>(at), std::min(kSector, g_boot_area.size() - at));
    if (!chunk) return std::unexpected(chunk.error());
    std::copy(chunk->begin(), chunk->end(), g_boot_area.begin() + static_cast<std::ptrdiff_t>(at));
  }
  return {};
}

std::string hex_digest(std::span<const std::uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0F]);
  }
  return out;
}

class Sha256 {
 public:
  Sha256() {
    (void)psa_crypto_init();
    (void)psa_hash_setup(&op_, PSA_ALG_SHA_256);
  }
  ~Sha256() {
    psa_hash_abort(&op_);
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(std::span<const std::uint8_t> bytes) {
    (void)psa_hash_update(&op_, bytes.data(), bytes.size());
  }
  std::string finish() {
    std::array<std::uint8_t, 32> digest{};
    std::size_t length = 0;
    (void)psa_hash_finish(&op_, digest.data(), digest.size(), &length);
    return hex_digest(std::span<const std::uint8_t>(digest.data(), length));
  }

 private:
  psa_hash_operation_t op_ = PSA_HASH_OPERATION_INIT;
};

std::vector<std::uint8_t> ufw_decrypt(std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> out(bytes.begin(), bytes.end());
  std::uint16_t key = 0xFFFF;
  for (auto& byte : out) {
    byte ^= static_cast<std::uint8_t>(key);
    key = static_cast<std::uint16_t>((key & 0x8000) != 0 ? (key << 1) ^ 0x1021 : key << 1);
  }
  return out;
}

std::uint16_t le16(std::span<const std::uint8_t> bytes, std::size_t at) {
  return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at) {
  return static_cast<std::uint32_t>(le16(bytes, at)) |
         (static_cast<std::uint32_t>(le16(bytes, at + 2)) << 16);
}

struct UfwFlash {
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

Result<UfwFlash> ufw_flash(package::LogicalImage& image) {
  if (image.size() < 0x400) return fail("package is shorter than its UFW header");
  std::vector<std::uint8_t> header(0x400);
  if (auto read = image.read(0, header); !read) return std::unexpected(read.error());
  const auto decoded = ufw_decrypt(std::span<const std::uint8_t>(header.data(), 0x40));
  if (wl82::crc16(std::span<const std::uint8_t>(decoded).subspan(2)) != le16(decoded, 0)) {
    return fail("UFW header CRC is invalid");
  }
  const auto entries = le16(decoded, 8);
  if (entries == 0 || entries > 11) return fail("UFW header has an invalid entry count");
  if (wl82::crc16(std::span<const std::uint8_t>(header).subspan(0x40, entries * 0x50)) !=
      le16(decoded, 2)) {
    return fail("UFW entry-list CRC is invalid");
  }
  UfwFlash flash;
  bool found = false;
  for (std::size_t i = 0; i < entries; ++i) {
    const auto entry =
        ufw_decrypt(std::span<const std::uint8_t>(header).subspan(0x40 + i * 0x50, 0x50));
    if (le16(entry, 0) != 0) continue;
    flash.offset = le32(entry, 8);
    flash.length = le32(entry, 12);
    found = true;
    break;
  }
  if (!found) return fail("UFW package has no flash.bin entry");
  if (flash.offset + flash.length > image.size()) {
    return fail("UFW flash.bin runs past the package");
  }
  if (flash.length < kAppEnd) return fail("flash.bin does not contain a complete 0x93000 image");
  return flash;
}

Result<std::string> region_hash(package::LogicalImage& image, const UfwFlash& flash) {
  Sha256 hash;
  std::vector<std::uint8_t> buffer(kSector);
  for (std::uint32_t at = kAppStart; at < kAppEnd; at += kSector) {
    if (auto read = image.read(flash.offset + at, buffer); !read) {
      return std::unexpected(read.error());
    }
    hash.update(buffer);
  }
  return hash.finish();
}

Result<std::string> flash_hash(wl82::Loader& loader) {
  Sha256 hash;
  for (std::uint32_t at = 0; at < wl82::kFlashSize; at += kSector) {
    auto data = loader.read(at, kSector);
    if (!data) return std::unexpected(data.error());
    hash.update(*data);
  }
  return hash.finish();
}

}  // namespace

Result<void> FlashWorker::start(UsbHost& usb, Storage& storage) {
  usb_ = &usb;
  storage_ = &storage;
  TaskHandle_t handle = nullptr;
  xTaskCreatePinnedToCore([](void* arg) { static_cast<FlashWorker*>(arg)->loop(); }, "fm1-flash",
                          8192, this, 3, &handle, 1);
  task_ = handle;
  return {};
}

bool FlashWorker::submit(FlashJob job) {
  {
    std::lock_guard lock(state_lock_);
    if (status_.running) return false;
    status_.running = true;
    status_.error = false;
    status_.step = "queued";
    status_.package = job.package_path;
    status_.percent = 0;
    status_.log.clear();
  }
  {
    std::lock_guard lock(job_lock_);
    jobs_.push_back(std::move(job));
  }
  job_wake_.notify_one();
  return true;
}

FlashStatus FlashWorker::status() const {
  std::lock_guard lock(state_lock_);
  return status_;
}

void FlashWorker::clear() {
  std::lock_guard lock(state_lock_);
  if (!status_.running) {
    status_ = {};
  }
}

void FlashWorker::publish(std::string step, int percent, bool error) {
  std::lock_guard lock(state_lock_);
  status_.step = std::move(step);
  if (percent >= 0) status_.percent = percent;
  if (error) {
    status_.error = true;
    status_.running = false;
  }
  status_.log.push_back(status_.step);
  if (status_.log.size() > 200) status_.log.erase(status_.log.begin());
}

void FlashWorker::loop() {
  for (;;) {
    FlashJob job;
    {
      std::unique_lock lock(job_lock_);
      job_wake_.wait(lock, [&] { return !jobs_.empty(); });
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    status_.error = false;
    status_.running = true;
    run(job);
    {
      std::lock_guard lock(state_lock_);
      status_.running = false;
    }
  }
}

void FlashWorker::run(const FlashJob& job) {
  Result<void> result;
  switch (job.kind) {
    case FlashKind::Normal:
      result = normal(job);
      break;
    case FlashKind::Rescue:
      result = rescue(job);
      break;
    case FlashKind::DownloadMode: {
      auto port = usb_->midi_port(job.slot);
      if (!port) {
        result = fail("the unit is no longer connected");
      } else if (!usb_->send_sysex(job.slot, fm1::midi::kDownloadMode)) {
        result = fail("could not send the download-mode command");
      } else {
        publish("waiting for the ROM disk", 0);
        bool appeared = false;
        for (int attempt = 0; attempt < 150; ++attempt) {
          if (!usb_->msc_ports().empty()) {
            appeared = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (appeared) {
          publish("ROM disk ready", 100);
        } else {
          result = fail("the ROM disk did not appear; reconnect and retry");
        }
      }
      break;
    }
    case FlashKind::InstallBootloader:
      result = install_bootloader(job);
      break;
    case FlashKind::Backup:
      result = backup(job);
      break;
    case FlashKind::Unbrick:
      result = unbrick(job);
      break;
    case FlashKind::InstallFirmware:
      result = install_firmware(job);
      break;
  }
  if (!result) {
    publish(result.error().message, -1, true);
  }
}

Result<void> FlashWorker::normal(const FlashJob& job) {
  auto port = usb_->midi_port(job.slot);
  if (!port) return fail("the unit is no longer connected");
  if (port->identity.empty()) {
    return fail("the device did not answer the handshake: use Rescue or check the cable");
  }
  // The update loader is already the second phase. Sending UPGRADE to it and
  // serving its write-finish address writes the image directly; the normal
  // stage-then-reboot flow would stop after the loader's first write request.
  if (port->ota) return rescue(job);

  FileReader reader;
  auto opened = reader.open(job.package_path);
  if (!opened) return opened;
  auto layout = package::inspect(reader);
  if (!layout) return std::unexpected(layout.error());
  package::LogicalImage image(reader, *layout);
  const std::string product = layout->product;
  const std::string model = model_of(product);
  if (!identity::package_fits(port->identity, product)) {
    return fail("the device is " + port->identity + ", the package is for " + product);
  }

  ClaimGuard guard{*usb_, job.slot};
  usb_->claim(job.slot, true);
  usb_->mute(job.slot, true);

  const std::string path = port->path;
  if (!usb_->send_sysex(job.slot, fm1::midi::kUpgrade)) {
    return fail("could not send the upgrade command");
  }
  vTaskDelay(pdMS_TO_TICKS(2000));
  publish("staging the update loader", 0);
  const auto stage_finish = std::array<uint32_t, 1>{fm1::midi::kFinishStage};
  auto stage = update::serve(
      image, [&](std::chrono::milliseconds timeout) { return usb_->read_sysex(job.slot, timeout); },
      [&](std::span<const uint8_t> sysex) { return usb_->send_sysex(job.slot, sysex); },
      [&](std::size_t done, std::size_t total) {
        publish("staging the update loader",
                static_cast<int>(done * 100 / std::max<std::size_t>(total, 1)));
      },
      [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); },
      {.finish_addresses = stage_finish,
       .idle_timeout = std::chrono::seconds(8),
       .pacing = std::chrono::milliseconds(10)});
  usb_->claim(job.slot, false);
  usb_->mute(job.slot, false);
  if (!stage) {
    return fail(stage.error().message + " (power-cycle the unit and retry)");
  }
  if (stage->finish != fm1::midi::kFinishStage) {
    return fail("the device stopped after " + std::to_string(stage->requests) +
                " requests; nothing was written");
  }

  publish("waiting for the update loader", 0);
  auto loader = usb_->wait_for_midi(path, std::chrono::seconds(90), [&](const MidiPort& p) {
    return is_ota(p) && model_of(identity::strip_ota(p.identity)) == model;
  });
  if (!loader) return fail("the update loader did not appear within 90 s");

  ClaimGuard loader_guard{*usb_, loader->slot};
  usb_->claim(loader->slot, true);
  usb_->mute(loader->slot, true);
  if (!usb_->send_sysex(loader->slot, fm1::midi::kUpgrade)) {
    return fail("could not send the upgrade command to the loader");
  }
  vTaskDelay(pdMS_TO_TICKS(2000));
  publish("writing the flash", 0);
  const auto write_finish = std::array<uint32_t, 1>{fm1::midi::kFinishWrite};
  auto write = update::serve(
      image,
      [&](std::chrono::milliseconds timeout) { return usb_->read_sysex(loader->slot, timeout); },
      [&](std::span<const uint8_t> sysex) { return usb_->send_sysex(loader->slot, sysex); },
      [&](std::size_t done, std::size_t total) {
        publish("writing the flash",
                static_cast<int>(done * 100 / std::max<std::size_t>(total, 1)));
      },
      [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); },
      {.finish_addresses = write_finish,
       .idle_timeout = std::chrono::seconds(180),
       .pacing = std::chrono::milliseconds(10)});
  if (!write) {
    return fail(write.error().message + " (power-cycle the unit and retry)");
  }
  if (write->finish != fm1::midi::kFinishWrite) {
    return fail("the update loader stopped after " + std::to_string(write->requests) +
                " requests; replug and press Flash again");
  }

  publish("written: waiting for the device", 0);
  auto back = usb_->wait_for_midi(path, std::chrono::seconds(60), [&](const MidiPort& p) {
    return !is_ota(p) && !p.identity.empty();
  });
  if (!back) return fail("the device did not come back within 60 s");
  if (!identity::same_identity(back->identity, product)) {
    return fail("the device came back as " + back->identity + ", not " + product);
  }
  publish("flashed: " + back->identity, 100);
  return {};
}

Result<void> FlashWorker::rescue(const FlashJob& job) {
  auto port = usb_->midi_port(job.slot);
  if (!port) return fail("the target port is gone");
  FileReader reader;
  auto opened = reader.open(job.package_path);
  if (!opened) return opened;
  auto layout = package::inspect(reader);
  if (!layout) return std::unexpected(layout.error());
  package::LogicalImage image(reader, *layout);
  const std::string product = layout->product;
  const std::string model = model_of(product);
  const std::string path = port->path;
  if (!port->identity.empty() && !identity::package_fits(port->identity, product)) {
    return fail("the device is " + port->identity + ", the package is for " + product);
  }

  ClaimGuard guard{*usb_, job.slot};
  usb_->claim(job.slot, true);
  usb_->mute(job.slot, true);
  if (!usb_->send_sysex(job.slot, fm1::midi::kUpgrade)) {
    return fail("could not send the upgrade command");
  }
  vTaskDelay(pdMS_TO_TICKS(2000));
  publish(port->ota ? "resuming the update loader" : "waiting for the device to pull the update",
          0);
  const auto both = std::array<uint32_t, 2>{fm1::midi::kFinishStage, fm1::midi::kFinishWrite};
  const auto write_only = std::array<uint32_t, 1>{fm1::midi::kFinishWrite};
  const std::span<const uint32_t> finish =
      port->ota ? std::span<const uint32_t>(write_only) : std::span<const uint32_t>(both);
  auto served = update::serve(
      image, [&](std::chrono::milliseconds timeout) { return usb_->read_sysex(job.slot, timeout); },
      [&](std::span<const uint8_t> sysex) { return usb_->send_sysex(job.slot, sysex); },
      [&](std::size_t done, std::size_t total) {
        publish("serving the image",
                static_cast<int>(done * 100 / std::max<std::size_t>(total, 1)));
      },
      [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); },
      {.finish_addresses = finish,
       .idle_timeout = std::chrono::seconds(8),
       .pacing = std::chrono::milliseconds(10)});
  if (!served) return std::unexpected(served.error());
  if (served->finish == fm1::midi::kFinishWrite) {
    if (port->ota) {
      publish("written: waiting for the device", 0);
      auto back = usb_->wait_for_midi(path, std::chrono::seconds(60), [&](const MidiPort& p) {
        return !is_ota(p) && !p.identity.empty();
      });
      if (!back) return fail("the device did not come back within 60 s");
      if (!identity::same_identity(back->identity, product)) {
        return fail("the device came back as " + back->identity + ", not " + product);
      }
      publish("flashed: " + back->identity, 100);
    } else {
      publish("flashed", 100);
    }
    return {};
  }
  if (served->finish != fm1::midi::kFinishStage) {
    return fail("the device stopped after " + std::to_string(served->requests) +
                " requests; nothing was written");
  }

  publish("waiting for the update loader", 0);
  auto loader = usb_->wait_for_midi(path, std::chrono::seconds(90), [&](const MidiPort& p) {
    return is_ota(p) && model_of(identity::strip_ota(p.identity)) == model;
  });
  if (!loader) return fail("the update loader did not appear within 90 s");
  ClaimGuard loader_guard{*usb_, loader->slot};
  usb_->claim(loader->slot, true);
  usb_->mute(loader->slot, true);
  if (!usb_->send_sysex(loader->slot, fm1::midi::kUpgrade)) {
    return fail("could not send the upgrade command to the loader");
  }
  vTaskDelay(pdMS_TO_TICKS(2000));
  const auto write_finish = std::array<uint32_t, 1>{fm1::midi::kFinishWrite};
  auto write = update::serve(
      image,
      [&](std::chrono::milliseconds timeout) { return usb_->read_sysex(loader->slot, timeout); },
      [&](std::span<const uint8_t> sysex) { return usb_->send_sysex(loader->slot, sysex); },
      [&](std::size_t done, std::size_t total) {
        publish("writing the flash",
                static_cast<int>(done * 100 / std::max<std::size_t>(total, 1)));
      },
      [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); },
      {.finish_addresses = write_finish,
       .idle_timeout = std::chrono::seconds(180),
       .pacing = std::chrono::milliseconds(10)});
  if (!write) return std::unexpected(write.error());
  if (write->finish != fm1::midi::kFinishWrite) {
    return fail("the update loader stopped before the write finished");
  }
  publish("flashed", 100);
  return {};
}

Result<void> FlashWorker::unbrick(const FlashJob& job) {
  return write_rom(job, true);
}

Result<void> FlashWorker::install_firmware(const FlashJob& job) {
  return write_rom(job, false);
}

Result<void> FlashWorker::write_rom(const FlashJob& job, bool v15_only) {
  auto msc = usb_->msc_port(job.slot);
  if (!msc) return fail("the WL82 UBOOT disk is not connected");
  FileReader reader;
  if (auto opened = reader.open(job.package_path); !opened) return opened;
  auto layout = package::inspect(reader);
  if (!layout) return std::unexpected(layout.error());
  package::LogicalImage image(reader, *layout);
  auto flash_part = ufw_flash(image);
  if (!flash_part) return std::unexpected(flash_part.error());
  auto expected = region_hash(image, *flash_part);
  if (!expected) return std::unexpected(expected.error());
  if (v15_only && *expected != kV15Hash) {
    return fail("DEBRICK requires the official FM-1 V15 firmware region");
  }

  MscTransport transport(*usb_, job.slot);
  wl82::Loader loader(transport, [&](std::string_view text) { publish(std::string(text)); });
  auto opened = loader.open(bytes(_binary_mflr_loader_a_bin_start, _binary_mflr_loader_a_bin_end));
  if (!opened) return opened;
  if (v15_only) {
    if (loader.chip_key() != kFm1ChipKey ||
        (loader.flash_id() != kFm1FlashId && loader.flash_id() != kFm1FlashId2)) {
      return fail("not an FM-1 flash (chip key or flash ID mismatch)");
    }
  } else {
    const std::string board = board_key_for(loader.flash_id());
    if (board.empty()) {
      return fail("the unit's model could not be identified; nothing was written");
    }
    const std::string package_model = model_of(layout->product);
    const bool model_fits =
        (board == "fm1" && (package_model == "FM-1" || package_model == "MiniFM")) ||
        (board == "smk37" && package_model.starts_with("SMK-37"));
    if (!model_fits) {
      return fail("the package is for " + package_model + ", the unit is " +
                  (board == "fm1" ? "FM-1" : "SMK-37 Pro / Elite"));
    }
  }

  (void)storage_->ensure_dir("fm1-card/backups");
  const std::string backup = storage_->path(
      "fm1-card/backups/" + std::string(v15_only ? "unbrick-" : "install-") + timestamp() + ".bin");
  std::FILE* dump = std::fopen(backup.c_str(), "wb");
  if (dump == nullptr) return fail("cannot create " + backup);

  publish("backing up the flash (1/2)", 0);
  Sha256 first;
  for (std::uint32_t at = 0; at < wl82::kFlashSize; at += kSector) {
    auto data = loader.read(at, kSector);
    if (!data) {
      std::fclose(dump);
      std::remove(backup.c_str());
      return std::unexpected(data.error());
    }
    if (std::fwrite(data->data(), 1, data->size(), dump) != data->size()) {
      std::fclose(dump);
      std::remove(backup.c_str());
      return fail("short backup write");
    }
    first.update(*data);
  }
  std::fclose(dump);
  const auto first_hash = first.finish();

  publish("backing up the flash (2/2)", 0);
  Sha256 second;
  for (std::uint32_t at = 0; at < wl82::kFlashSize; at += kSector) {
    auto data = loader.read(at, kSector);
    if (!data) return std::unexpected(data.error());
    second.update(*data);
  }
  if (second.finish() != first_hash) {
    return fail("the two flash reads differ; nothing was written (backup: " + backup + ")");
  }
  publish("backup verified: " + backup, 0);

  publish("checking that the flash is unchanged", 0);
  auto current = flash_hash(loader);
  if (!current) return std::unexpected(current.error());
  if (*current != first_hash) {
    return fail("the flash changed after the backup; nothing was written (backup: " + backup + ")");
  }

  publish("erasing the firmware region", 0);
  if (auto erased = loader.erase(kAppStart, kAppEnd - kAppStart); !erased) {
    return std::unexpected(erased.error());
  }

  std::vector<std::uint8_t> sector(kSector);
  for (std::uint32_t at = kAppStart; at < kAppEnd; at += kSector) {
    if (auto read = image.read(flash_part->offset + at, sector); !read) {
      return std::unexpected(read.error());
    }
    if (auto written = loader.write(at, sector); !written) {
      return std::unexpected(written.error());
    }
    auto back = loader.read(at, kSector);
    if (!back) return std::unexpected(back.error());
    if (back->size() != sector.size() || !std::equal(back->begin(), back->end(), sector.begin())) {
      return fail("readback mismatch at 0x" + std::to_string(at));
    }
    publish("writing 0x4000..0x92FFF",
            static_cast<int>((at - kAppStart + kSector) * 100 / (kAppEnd - kAppStart)));
  }

  publish("verifying the restored firmware", 100);
  Sha256 firmware;
  for (std::uint32_t at = kAppStart; at < kAppEnd; at += kSector) {
    auto data = loader.read(at, kSector);
    if (!data) return std::unexpected(data.error());
    firmware.update(*data);
  }
  if (firmware.finish() != *expected) {
    return fail(v15_only ? "restored firmware hash mismatch" : "written firmware hash mismatch");
  }
  publish(v15_only ? "debrick complete: official V15 restored"
                   : "firmware installed: " + layout->product,
          100);
  return {};
}

Result<void> FlashWorker::backup(const FlashJob& job) {
  auto msc = usb_->msc_port(job.slot);
  if (!msc) return fail("the ROM disk is no longer connected");
  MscTransport transport(*usb_, job.slot);
  wl82::Loader loader(transport, [&](std::string_view text) { publish(std::string(text)); });
  auto opened = loader.open(bytes(_binary_mflr_loader_a_bin_start, _binary_mflr_loader_a_bin_end));
  if (!opened) return opened;

  const std::string path = job.output_path.empty()
                               ? storage_->path("fm1-card/backups/backup-" + timestamp() + ".bin")
                               : job.output_path;
  (void)storage_->ensure_dir("fm1-card/backups");
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return fail("cannot create " + path);

  uint32_t crc = 0;
  constexpr std::size_t kChunk = 4 * 1024;
  for (std::size_t at = 0; at < wl82::kFlashSize; at += kChunk) {
    auto chunk = loader.read(static_cast<uint32_t>(at), kChunk);
    if (!chunk) {
      std::fclose(file);
      return std::unexpected(chunk.error());
    }
    if (std::fwrite(chunk->data(), 1, chunk->size(), file) != chunk->size()) {
      std::fclose(file);
      return fail("short write to the backup");
    }
    crc = crc32_update(crc, *chunk);
    publish("reading the flash", static_cast<int>((at + chunk->size()) * 100 / wl82::kFlashSize));
  }
  std::fflush(file);
  std::fclose(file);
  auto chip_crc = loader.crc32_of(0, wl82::kFlashSize);
  if (!chip_crc) return std::unexpected(chip_crc.error());
  if (*chip_crc != crc) {
    return fail("the flash read did not match the chip checksum");
  }
  publish("backup saved: " + path, 100);
  return {};
}

Result<void> FlashWorker::install_bootloader(const FlashJob& job) {
  auto msc = usb_->msc_port(job.slot);
  if (!msc) return fail("the ROM disk is no longer connected");
  MscTransport transport(*usb_, job.slot);
  wl82::Loader loader(transport, [&](std::string_view text) { publish(std::string(text)); });
  auto opened = loader.open(bytes(_binary_mflr_loader_a_bin_start, _binary_mflr_loader_a_bin_end));
  if (!opened) return opened;

  if (auto read = read_boot_area(loader); !read) return read;
  const std::string board = board_key_for(loader.flash_id());
  if (board.empty()) {
    return fail("the unit's model could not be identified; nothing was written");
  }

  const std::string backup_path =
      job.output_path.empty()
          ? storage_->path("fm1-card/backups/" + board + "-" + timestamp() + ".bin")
          : job.output_path;
  FlashJob read_job = job;
  read_job.output_path = backup_path;
  auto saved = backup(read_job);
  if (!saved) return saved;

  std::span<const uint8_t> bootloader;
  if (board == "fm1") {
    bootloader = bytes(_binary_bootloader_fm1_boot_start, _binary_bootloader_fm1_boot_end);
  } else {
    bootloader = bytes(_binary_bootloader_smk37_boot_start, _binary_bootloader_smk37_boot_end);
  }

  auto area = flash::build_boot_area(g_boot_area, bootloader, g_boot_build);
  if (!area) return area;
  publish("erasing the boot area", 0);
  auto erased = loader.erase(0, flash::kBootArea);
  if (!erased) return erased;
  publish("writing the boot area", 50);
  auto written = loader.write(0, g_boot_build);
  if (!written) return written;
  publish("verifying the boot area", 90);
  if (auto read = read_boot_area(loader); !read) return read;
  if (g_boot_area != g_boot_build) {
    return fail("the boot area did not verify; do not power off, reconnect and retry");
  }
  publish("rescue bootloader installed", 100);
  return {};
}

}  // namespace fm1::platform

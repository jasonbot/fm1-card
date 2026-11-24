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
        publish("download mode requested: the ROM disk will appear");
      }
      break;
    }
    case FlashKind::InstallBootloader:
      result = install_bootloader(job);
      break;
    case FlashKind::Backup:
      result = backup(job);
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
  usb_->send_sysex(job.slot, fm1::midi::kUpgrade);
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
  usb_->send_sysex(loader->slot, fm1::midi::kUpgrade);
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

  ClaimGuard guard{*usb_, job.slot};
  usb_->claim(job.slot, true);
  usb_->mute(job.slot, true);
  usb_->send_sysex(job.slot, fm1::midi::kUpgrade);
  vTaskDelay(pdMS_TO_TICKS(2000));
  publish("waiting for the device to pull the update", 0);
  const auto both = std::array<uint32_t, 2>{fm1::midi::kFinishStage, fm1::midi::kFinishWrite};
  auto served = update::serve(
      image, [&](std::chrono::milliseconds timeout) { return usb_->read_sysex(job.slot, timeout); },
      [&](std::span<const uint8_t> sysex) { return usb_->send_sysex(job.slot, sysex); },
      [&](std::size_t done, std::size_t total) {
        publish("serving the image",
                static_cast<int>(done * 100 / std::max<std::size_t>(total, 1)));
      },
      [](std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); },
      {.finish_addresses = both,
       .idle_timeout = std::chrono::seconds(8),
       .pacing = std::chrono::milliseconds(10)});
  if (!served) return std::unexpected(served.error());
  if (served->finish == fm1::midi::kFinishWrite) {
    publish("flashed", 100);
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
  usb_->send_sysex(loader->slot, fm1::midi::kUpgrade);
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
  constexpr std::size_t kChunk = 64 * 1024;
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

  auto boot_area = loader.read(0, flash::kBootArea);
  if (!boot_area) return std::unexpected(boot_area.error());
  auto analysis = flash::analyze(*boot_area, loader.flash_id(), loader.chip_key());
  if (!analysis.model_known) {
    return fail("the unit's model could not be identified; nothing was written");
  }

  const std::string backup_path =
      job.output_path.empty()
          ? storage_->path("fm1-card/backups/" + analysis.board_key + "-" + timestamp() + ".bin")
          : job.output_path;
  FlashJob read_job = job;
  read_job.output_path = backup_path;
  auto saved = backup(read_job);
  if (!saved) return saved;

  std::span<const uint8_t> bootloader;
  if (analysis.board_key == "fm1") {
    bootloader = bytes(_binary_bootloader_fm1_boot_start, _binary_bootloader_fm1_boot_end);
  } else if (analysis.board_key == "smk37") {
    bootloader = bytes(_binary_bootloader_smk37_boot_start, _binary_bootloader_smk37_boot_end);
  } else {
    return fail("no bootloader for " + analysis.board_key);
  }

  auto area = flash::build_boot_area(*boot_area, bootloader);
  if (!area) return std::unexpected(area.error());
  publish("erasing the boot area", 0);
  auto erased = loader.erase(0, flash::kBootArea);
  if (!erased) return erased;
  publish("writing the boot area", 50);
  auto written = loader.write(0, *area);
  if (!written) return written;
  publish("verifying the boot area", 90);
  auto verify = loader.read(0, flash::kBootArea);
  if (!verify) return std::unexpected(verify.error());
  if (*verify != *area) {
    return fail("the boot area did not verify; do not power off, reconnect and retry");
  }
  publish("rescue bootloader installed", 100);
  return {};
}

}  // namespace fm1::platform

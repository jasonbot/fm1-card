#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "fm1/platform/board.hpp"
#include "fm1/platform/usb.hpp"
#include "fm1/result.hpp"

namespace fm1::platform {

enum class FlashKind : uint8_t {
  Normal,
  Rescue,
  DownloadMode,
  InstallBootloader,
  Backup,
  Unbrick,
};

struct FlashJob {
  FlashKind kind = FlashKind::Normal;
  uint8_t slot = 0;
  std::string package_path;
  std::string output_path;
};

struct FlashStatus {
  bool running = false;
  bool error = false;
  std::string step;
  std::string package;
  int percent = 0;
  std::vector<std::string> log;
};

class FlashWorker {
 public:
  Result<void> start(UsbHost& usb, Storage& storage);
  bool submit(FlashJob job);
  [[nodiscard]] FlashStatus status() const;
  void clear();

 private:
  void loop();
  void publish(std::string step, int percent = -1, bool error = false);
  void run(const FlashJob& job);
  Result<void> normal(const FlashJob& job);
  Result<void> rescue(const FlashJob& job);
  Result<void> unbrick(const FlashJob& job);
  Result<void> install_bootloader(const FlashJob& job);
  Result<void> backup(const FlashJob& job);

  UsbHost* usb_ = nullptr;
  Storage* storage_ = nullptr;
  void* task_ = nullptr;
  mutable std::mutex state_lock_;
  FlashStatus status_;
  std::mutex job_lock_;
  std::condition_variable job_wake_;
  std::deque<FlashJob> jobs_;
};

}  // namespace fm1::platform

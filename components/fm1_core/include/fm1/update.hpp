#pragma once

// The device-pulls-image half of the FM-1 update protocol. The platform gives
// this a blocking "next SysEx" reader and a "send SysEx" writer; the loop is
// identical to the desktop hub installer, including its 10 ms pacing.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "fm1/package.hpp"
#include "fm1/result.hpp"

namespace fm1::update {

using ReadFn =
    std::function<std::optional<std::vector<uint8_t>>(std::chrono::milliseconds timeout)>;
using SendFn = std::function<bool(std::span<const uint8_t> sysex)>;
using ProgressFn = std::function<void(std::size_t served, std::size_t total)>;
using DelayFn = std::function<void(std::chrono::milliseconds)>;

struct ServeResult {
  uint32_t requests = 0;
  std::optional<uint32_t> finish;
};

struct ServeOptions {
  std::span<const uint32_t> finish_addresses{};
  std::chrono::milliseconds idle_timeout{std::chrono::seconds(8)};
  std::chrono::milliseconds pacing{std::chrono::milliseconds(10)};
};

// Answers read requests until one of the finish addresses arrives or the
// device has been idle for idle_timeout.
Result<ServeResult> serve(package::LogicalImage& image, const ReadFn& read, const SendFn& send,
                          const ProgressFn& progress, const DelayFn& delay,
                          const ServeOptions& options);

}  // namespace fm1::update

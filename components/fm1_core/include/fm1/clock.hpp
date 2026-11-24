#pragma once

// MIDI clock scheduling, no threads and no hardware. The platform task sleeps
// until next_due_us(), sends 0xF8, and calls advance().

#include <cstdint>
#include <optional>

namespace fm1::clock {

constexpr uint32_t kMinBpm = 20;
constexpr uint32_t kMaxBpm = 300;
constexpr uint32_t kDefaultBpm = 120;
constexpr uint32_t kTicksPerBeat = 24;

class Clock {
 public:
  explicit Clock(uint32_t bpm = kDefaultBpm) : bpm_(clamp(bpm)) {}

  [[nodiscard]] uint32_t bpm() const {
    return bpm_;
  }
  void set_bpm(uint32_t bpm);

  [[nodiscard]] bool running() const {
    return running_;
  }
  [[nodiscard]] uint64_t tick() const {
    return tick_;
  }
  [[nodiscard]] uint64_t beat() const {
    return tick_ / kTicksPerBeat;
  }

  [[nodiscard]] uint64_t interval_us() const {
    return 60'000'000ull / (static_cast<uint64_t>(bpm_) * kTicksPerBeat);
  }

  // Start sends MIDI Start (0xFA) at the caller; the first clock tick is due
  // immediately.
  void start(uint64_t now_us);
  void stop();
  void restart_phase(uint64_t now_us);

  [[nodiscard]] std::optional<uint64_t> next_due_us() const;
  // Move to the next tick. If the scheduler is more than one interval late,
  // resynchronise instead of sending a burst.
  void advance(uint64_t now_us);

 private:
  static constexpr uint32_t clamp(uint32_t bpm) {
    return bpm < kMinBpm ? kMinBpm : (bpm > kMaxBpm ? kMaxBpm : bpm);
  }

  uint32_t bpm_ = kDefaultBpm;
  bool running_ = false;
  uint64_t tick_ = 0;
  uint64_t due_us_ = 0;
};

}  // namespace fm1::clock

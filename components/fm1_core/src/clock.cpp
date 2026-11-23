#include "fm1/clock.hpp"

namespace fm1::clock {

void Clock::set_bpm(uint32_t bpm) {
  bpm_ = clamp(bpm);
}

void Clock::start(uint64_t now_us) {
  running_ = true;
  due_us_ = now_us;
}

void Clock::stop() {
  running_ = false;
}

void Clock::restart_phase(uint64_t now_us) {
  tick_ = 0;
  due_us_ = now_us;
}

std::optional<uint64_t> Clock::next_due_us() const {
  if (!running_) {
    return std::nullopt;
  }
  return due_us_;
}

void Clock::advance(uint64_t now_us) {
  if (!running_) {
    return;
  }
  ++tick_;
  due_us_ += interval_us();
  if (now_us > due_us_ + interval_us()) {
    // We fell behind (Wi-Fi, flash, a long USB transfer). Do not send a
    // catch-up burst; restart the phase from now.
    due_us_ = now_us + interval_us();
  }
}

}  // namespace fm1::clock

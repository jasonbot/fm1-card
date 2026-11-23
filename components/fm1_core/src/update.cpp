#include "fm1/update.hpp"

#include <algorithm>
#include <array>

#include "fm1/midi.hpp"

namespace fm1::update {

Result<ServeResult> serve(package::LogicalImage& image, const ReadFn& read, const SendFn& send,
                          const ProgressFn& progress, const DelayFn& delay,
                          const ServeOptions& options) {
  ServeResult result;
  std::size_t served_bytes = 0;
  std::size_t last_percent = static_cast<std::size_t>(-1);
  const std::size_t total = static_cast<std::size_t>(image.size());
  std::array<uint8_t, midi::kMaxData> buffer{};
  const auto idle = options.idle_timeout;

  while (true) {
    auto packet = read(idle);
    if (!packet) {
      // The platform reader waits for the idle deadline and returns nullopt
      // when nothing arrived.
      return result;
    }
    const auto request = midi::parse_request(*packet);
    if (!request) {
      continue;
    }
    if (request->address == midi::kFinishStage || request->address == midi::kFinishWrite) {
      constexpr std::array<uint8_t, 8> kSuccess{'s', 'u', 'c', 'c', 'e', 's', 's', 0};
      if (!send(midi::response(request->address, kSuccess, 0))) {
        return fail("the device was disconnected while acknowledging the finish");
      }
      if (std::find(options.finish_addresses.begin(), options.finish_addresses.end(),
                    request->address) != options.finish_addresses.end()) {
        result.finish = request->address;
        return result;
      }
      continue;
    }
    if (request->length > midi::kMaxData ||
        static_cast<uint64_t>(request->address) + request->length > image.size()) {
      return fail("the device requested a range outside the firmware image");
    }

    if (options.pacing.count() > 0) {
      delay(options.pacing);
    }

    const auto span = std::span<uint8_t>(buffer.data(), request->length);
    auto got = image.read(request->address, span);
    if (!got) {
      return std::unexpected(got.error());
    }
    if (!send(midi::response(request->address, span, request->flag))) {
      return fail("the device was disconnected mid-update");
    }

    ++result.requests;
    served_bytes += request->length;
    const std::size_t percent = served_bytes * 100 / std::max<std::size_t>(total, 1);
    if (percent != last_percent) {
      last_percent = percent;
      if (progress) {
        progress(served_bytes, total);
      }
    }
  }
}

}  // namespace fm1::update

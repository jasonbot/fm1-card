#pragma once

// A tiny MIDI graph: 8 logical USB-MIDI ports, one sink bitmask per source.
// Forwarding is raw USB-MIDI event packets, so SysEx and real-time bytes keep
// their original packetisation. The platform wraps this in a mutex and owns
// the actual transfer.

#include <array>
#include <cstdint>

namespace fm1::routing {

constexpr uint8_t kMaxPorts = 8;
using Mask = uint8_t;
using PortId = uint8_t;

struct Link {
  PortId source = 0;
  PortId sink = 0;
};

class Router {
 public:
  [[nodiscard]] Mask sinks(PortId source) const {
    return source < kMaxPorts ? sinks_[source] : 0;
  }

  [[nodiscard]] bool forwards(PortId source, PortId sink) const {
    return sink < kMaxPorts && (sinks(source) & (1u << sink)) != 0;
  }

  bool connect(PortId source, PortId sink);
  bool disconnect(PortId source, PortId sink);
  void toggle(PortId source, PortId sink);
  void clear();

  // Drop every link into or out of a port (unplug).
  void remove_port(PortId port);

  [[nodiscard]] bool clock_enabled(PortId port) const {
    return port < kMaxPorts && clock_[port];
  }
  void set_clock(PortId port, bool enabled);
  void set_clock_all(bool enabled);
  [[nodiscard]] bool any_clock() const;

  // All enabled non-source ports, as a mask.
  [[nodiscard]] Mask clock_targets() const;

 private:
  std::array<Mask, kMaxPorts> sinks_{};
  std::array<bool, kMaxPorts> clock_{};
};

}  // namespace fm1::routing

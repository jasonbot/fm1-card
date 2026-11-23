#include "fm1/routing.hpp"

namespace fm1::routing {

bool Router::connect(PortId source, PortId sink) {
  if (source >= kMaxPorts || sink >= kMaxPorts || source == sink) {
    return false;
  }
  const Mask bit = static_cast<Mask>(1u << sink);
  if ((sinks_[source] & bit) != 0) {
    return false;
  }
  sinks_[source] |= bit;
  return true;
}

bool Router::disconnect(PortId source, PortId sink) {
  if (source >= kMaxPorts || sink >= kMaxPorts) {
    return false;
  }
  const Mask bit = static_cast<Mask>(1u << sink);
  if ((sinks_[source] & bit) == 0) {
    return false;
  }
  sinks_[source] &= static_cast<Mask>(~bit);
  return true;
}

void Router::toggle(PortId source, PortId sink) {
  if (forwards(source, sink)) {
    disconnect(source, sink);
  } else {
    connect(source, sink);
  }
}

void Router::clear() {
  sinks_.fill(0);
  clock_.fill(false);
}

void Router::remove_port(PortId port) {
  if (port >= kMaxPorts) {
    return;
  }
  const Mask bit = static_cast<Mask>(1u << port);
  for (auto& mask : sinks_) {
    mask &= static_cast<Mask>(~bit);
  }
  sinks_[port] = 0;
  clock_[port] = false;
}

void Router::set_clock(PortId port, bool enabled) {
  if (port < kMaxPorts) {
    clock_[port] = enabled;
  }
}

void Router::set_clock_all(bool enabled) {
  clock_.fill(enabled);
}

bool Router::any_clock() const {
  for (bool enabled : clock_) {
    if (enabled) {
      return true;
    }
  }
  return false;
}

Mask Router::clock_targets() const {
  Mask mask = 0;
  for (PortId port = 0; port < kMaxPorts; ++port) {
    if (clock_[port]) {
      mask |= static_cast<Mask>(1u << port);
    }
  }
  return mask;
}

}  // namespace fm1::routing

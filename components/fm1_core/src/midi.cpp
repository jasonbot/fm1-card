#include "fm1/midi.hpp"

#include <algorithm>

namespace fm1::midi {
namespace {

constexpr std::size_t kSysexLimit = 8192;

bool is_status(uint8_t byte) {
  return (byte & 0x80) != 0;
}

}  // namespace

std::vector<uint8_t> pack7(std::span<const uint8_t> data) {
  std::vector<uint8_t> out;
  out.reserve(data.size() * 8 / 7 + 1);
  uint32_t accumulator = 0;
  uint32_t bits = 0;
  for (uint8_t byte : data) {
    accumulator |= static_cast<uint32_t>(byte) << bits;
    bits += 8;
    while (bits >= 7) {
      out.push_back(static_cast<uint8_t>(accumulator & 0x7F));
      accumulator >>= 7;
      bits -= 7;
    }
  }
  if (bits != 0) {
    out.push_back(static_cast<uint8_t>(accumulator & 0x7F));
  }
  return out;
}

std::vector<uint8_t> unpack7(std::span<const uint8_t> data) {
  std::vector<uint8_t> out;
  out.reserve(data.size() * 7 / 8 + 1);
  uint32_t accumulator = 0;
  uint32_t bits = 0;
  for (uint8_t byte : data) {
    accumulator |= static_cast<uint32_t>(byte & 0x7F) << bits;
    bits += 7;
    while (bits >= 8) {
      out.push_back(static_cast<uint8_t>(accumulator & 0xFF));
      accumulator >>= 8;
      bits -= 8;
    }
  }
  return out;
}

std::vector<Packet> encode_message(uint8_t cable, std::span<const uint8_t> message) {
  std::vector<Packet> packets;
  if (message.empty()) {
    return packets;
  }
  const uint8_t head = static_cast<uint8_t>(cable << 4);

  if (message.front() == 0xF0) {
    std::size_t at = 0;
    while (at < message.size()) {
      const std::size_t chunk = std::min<std::size_t>(3, message.size() - at);
      Packet packet{};
      packet.b0 = message[at];
      packet.b1 = chunk > 1 ? message[at + 1] : 0;
      packet.b2 = chunk > 2 ? message[at + 2] : 0;
      const bool last = at + chunk >= message.size();
      uint8_t cin = 0x4;
      if (last) {
        cin = chunk == 1 ? 0x5 : chunk == 2 ? 0x6 : 0x7;
      }
      packet.head = static_cast<uint8_t>(head | cin);
      packets.push_back(packet);
      at += chunk;
    }
    return packets;
  }

  const uint8_t status = message.front();
  std::size_t length = 3;
  uint8_t cin = status >> 4;
  if (status >= 0xF0) {
    switch (status) {
      case 0xF1:
      case 0xF3:
        length = 2;
        cin = 0x2;
        break;
      case 0xF2:
        length = 3;
        cin = 0x3;
        break;
      case 0xF6:
      case 0xF8:
      case 0xF9:
      case 0xFA:
      case 0xFB:
      case 0xFC:
      case 0xFD:
      case 0xFE:
      case 0xFF:
        length = 1;
        cin = 0xF;
        break;
      default:
        length = 1;
        cin = 0xF;
        break;
    }
  } else if (status >= 0xC0 && status <= 0xDF) {
    length = 2;
  }
  length = std::min(length, message.size());

  Packet packet{};
  packet.head = static_cast<uint8_t>(head | cin);
  packet.b0 = message[0];
  if (length > 1) packet.b1 = message[1];
  if (length > 2) packet.b2 = message[2];
  packets.push_back(packet);
  return packets;
}

std::optional<std::vector<uint8_t>> SysexAssembler::feed(const Packet& packet) {
  const uint8_t cin = packet.cin();
  if (cin == 0xF) {
    return std::nullopt;  // real-time byte, not part of a SysEx
  }
  if (cin == 0x4) {
    const auto payload = packet.payload();
    if (!payload.empty() && payload.front() == 0xF0) {
      buffer_.clear();
      active_ = true;
    }
    if (!active_) {
      return std::nullopt;
    }
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());
  } else if (cin >= 0x5 && cin <= 0x7) {
    if (!active_) {
      return std::nullopt;
    }
    const auto payload = packet.payload();
    buffer_.insert(buffer_.end(), payload.begin(), payload.end());
  } else {
    return std::nullopt;
  }

  if (buffer_.size() > kSysexLimit) {
    buffer_.clear();
    active_ = false;
    return std::nullopt;
  }

  const bool ended = !buffer_.empty() && buffer_.back() == 0xF7;
  if (!ended) {
    return std::nullopt;
  }
  std::vector<uint8_t> message = std::move(buffer_);
  buffer_.clear();
  active_ = false;
  return message;
}

std::optional<std::string> parse_identity(std::span<const uint8_t> sysex) {
  if (sysex.size() < 3 || sysex.front() != 0xF0 || sysex.back() != 0xF7) {
    return std::nullopt;
  }
  const auto unpacked = unpack7(sysex.subspan(1, sysex.size() - 2));
  if (unpacked.size() != 34 || unpacked[0] != 0 || unpacked[1] != 0x59 || unpacked[2] != 0x11) {
    return std::nullopt;
  }
  std::string text;
  for (std::size_t i = 6; i < 33 && unpacked[i] != 0; ++i) {
    text.push_back(static_cast<char>(unpacked[i]));
  }
  const auto underscore = text.find('_');
  if (underscore == std::string::npos || underscore == 0 || underscore + 1 >= text.size()) {
    return std::nullopt;
  }
  return text;
}

std::optional<Request> parse_request(std::span<const uint8_t> sysex) {
  if (sysex.size() < 3 || sysex.front() != 0xF0 || sysex.back() != 0xF7) {
    return std::nullopt;
  }
  const auto unpacked = unpack7(sysex.subspan(1, sysex.size() - 2));
  if (unpacked.size() != 15 || unpacked[0] != 0 || unpacked[1] != 0x59 || unpacked[2] != 0x30) {
    return std::nullopt;
  }
  uint32_t sum = 0;
  for (std::size_t i = 6; i < 14; ++i) {
    sum += unpacked[i];
  }
  if (static_cast<uint8_t>(~sum & 0xFF) != unpacked[14]) {
    return std::nullopt;
  }
  return Request{
      .flag = unpacked[6],
      .address = static_cast<uint32_t>(unpacked[7]) | (static_cast<uint32_t>(unpacked[8]) << 8) |
                 (static_cast<uint32_t>(unpacked[9]) << 16) |
                 (static_cast<uint32_t>(unpacked[10]) << 24),
      .length = static_cast<uint32_t>(unpacked[11]) | (static_cast<uint32_t>(unpacked[12]) << 8) |
                (static_cast<uint32_t>(unpacked[13]) << 16),
  };
}

std::vector<uint8_t> response(uint32_t address, std::span<const uint8_t> data, uint8_t flag) {
  const std::size_t length = data.size();
  std::vector<uint8_t> body;
  body.reserve(15 + length);
  body.insert(body.end(), {
                              0x00,
                              0x59,
                              0x30,
                              static_cast<uint8_t>((length + 8) & 0xFF),
                              static_cast<uint8_t>(((length + 8) >> 8) & 0xFF),
                              0,
                              flag,
                              static_cast<uint8_t>(address & 0xFF),
                              static_cast<uint8_t>((address >> 8) & 0xFF),
                              static_cast<uint8_t>((address >> 16) & 0xFF),
                              static_cast<uint8_t>((address >> 24) & 0xFF),
                              static_cast<uint8_t>(length & 0xFF),
                              static_cast<uint8_t>((length >> 8) & 0xFF),
                              0,
                          });
  body.insert(body.end(), data.begin(), data.end());
  uint32_t sum = 0;
  for (std::size_t i = 6; i < body.size(); ++i) {
    sum += body[i];
  }
  body.push_back(static_cast<uint8_t>(~sum & 0xFF));

  std::vector<uint8_t> sysex;
  sysex.reserve(body.size() + 2);
  sysex.push_back(0xF0);
  const auto packed = pack7(body);
  sysex.insert(sysex.end(), packed.begin(), packed.end());
  sysex.push_back(0xF7);
  return sysex;
}

}  // namespace fm1::midi

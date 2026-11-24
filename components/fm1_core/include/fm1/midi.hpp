#pragma once

// USB-MIDI 1.0 event packets, the FM-1 SysEx handshake/update protocol, and the
// 7-bit packing both sides use. No allocation on the hot path: packets are
// fixed 4-byte values and SysEx assembly uses a caller-owned buffer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fm1::midi {

struct Packet {
  uint8_t head = 0;  // cable << 4 | CIN
  uint8_t b0 = 0;
  uint8_t b1 = 0;
  uint8_t b2 = 0;

  [[nodiscard]] constexpr uint8_t cin() const {
    return head & 0x0F;
  }
  [[nodiscard]] constexpr uint8_t cable() const {
    return head >> 4;
  }
  [[nodiscard]] constexpr std::size_t payload_size() const;
  [[nodiscard]] constexpr std::span<const uint8_t> payload() const {
    return {&b0, payload_size()};
  }
  constexpr bool operator==(const Packet&) const = default;
};

constexpr std::size_t Packet::payload_size() const {
  switch (cin()) {
    case 0x2:  // 2-byte system common
    case 0x6:  // SysEx end, 2 bytes
    case 0xC:
    case 0xD:
      return 2;
    case 0x3:  // 3-byte system common
    case 0x4:  // SysEx start/continue
    case 0x7:  // SysEx end, 3 bytes
    case 0x8:
    case 0x9:
    case 0xA:
    case 0xB:
    case 0xE:
      return 3;
    default:
      return 1;
  }
}

constexpr std::array<uint8_t, 10> kHandshakeQuery{0xF0, 0x00, 0x32, 0x45, 0x00,
                                                  0x00, 0x00, 0x40, 0x7F, 0xF7};
constexpr std::array<uint8_t, 6> kUpgrade{0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7};
constexpr std::array<uint8_t, 6> kDownloadMode{0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7};

constexpr uint32_t kFinishStage = 0xE000'0000;
constexpr uint32_t kFinishWrite = 0xF000'0000;
constexpr std::size_t kMaxData = 512;

// 8-bit data -> 7-bit words, little-endian bit stream.
std::vector<uint8_t> pack7(std::span<const uint8_t> data);
std::vector<uint8_t> unpack7(std::span<const uint8_t> data);

// Encode one complete MIDI message (or one real-time byte) as USB-MIDI
// packets. SysEx is split into CIN 4/5/6/7 events.
std::vector<Packet> encode_message(uint8_t cable, std::span<const uint8_t> message);

// Reassembles SysEx from packets. Call feed() for every packet; it returns a
// complete F0..F7 message when the end CIN arrives. Non-SysEx packets are
// ignored (they are forwarded as raw packets elsewhere).
class SysexAssembler {
 public:
  std::optional<std::vector<uint8_t>> feed(const Packet& packet);

 private:
  std::vector<uint8_t> buffer_;
  bool active_ = false;
};

// A handshake response: "MODEL_VERSION" (e.g. FM-1_916, ota-FM-1_904).
std::optional<std::string> parse_identity(std::span<const uint8_t> sysex);

// One read request emitted by the FM-1 update loader.
struct Request {
  uint8_t flag = 0;
  uint32_t address = 0;
  uint32_t length = 0;
};

std::optional<Request> parse_request(std::span<const uint8_t> sysex);

// Answer a read request: header 00 59 30, flag, address, length, data, sum.
std::vector<uint8_t> response(uint32_t address, std::span<const uint8_t> data, uint8_t flag);

}  // namespace fm1::midi

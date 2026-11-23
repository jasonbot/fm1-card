#include "fm1/wl82.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <thread>

namespace fm1::wl82 {
namespace {

constexpr std::array<uint8_t, 16> kMagic{
    0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
    0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6};

uint16_t crc16_update(uint16_t crc, std::span<const uint8_t> bytes) {
  for (uint8_t byte : bytes) {
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) != 0 ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

std::vector<uint8_t> sized_args(uint32_t address, std::span<const uint8_t> bytes) {
  std::vector<uint8_t> args;
  args.reserve(9);
  args.push_back(static_cast<uint8_t>(address >> 24));
  args.push_back(static_cast<uint8_t>(address >> 16));
  args.push_back(static_cast<uint8_t>(address >> 8));
  args.push_back(static_cast<uint8_t>(address));
  args.push_back(static_cast<uint8_t>(bytes.size() >> 8));
  args.push_back(static_cast<uint8_t>(bytes.size()));
  args.push_back(0);
  const uint16_t crc = crc16(bytes);
  args.push_back(static_cast<uint8_t>(crc));
  args.push_back(static_cast<uint8_t>(crc >> 8));
  return args;
}

template <typename T>
Result<T> retry(const LogFn& log, std::function<Result<T>()> action) {
  std::optional<Error> last;
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto value = action();
    if (value) {
      return value;
    }
    last = value.error();
    if (log) {
      log("retrying: " + last->message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150 * (attempt + 1)));
  }
  return std::unexpected(*last);
}


}  // namespace

uint16_t crc16(std::span<const uint8_t> bytes) { return crc16_update(0, bytes); }

uint32_t crc32(std::span<const uint8_t> bytes) {
  std::array<uint32_t, 256> table{};
  for (std::size_t i = 0; i < table.size(); ++i) {
    uint32_t value = static_cast<uint32_t>(i);
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1) != 0 ? 0xEDB8'8320u ^ (value >> 1) : value >> 1;
    }
    table[i] = value;
  }
  uint32_t crc = ~0u;
  for (uint8_t byte : bytes) {
    crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

std::array<uint8_t, 16> cdb(uint16_t command, std::span<const uint8_t> args) {
  std::array<uint8_t, 16> packet{};
  packet.fill(0xFF);
  packet[0] = static_cast<uint8_t>(command >> 8);
  packet[1] = static_cast<uint8_t>(command);
  std::copy_n(args.begin(), std::min<std::size_t>(args.size(), 14), packet.begin() + 2);
  return packet;
}

std::vector<uint8_t> cipher(std::span<const uint8_t> bytes, uint32_t key) {
  // Seed with the low half, then feed the high half as two little-endian
  // bytes: crc16_update(key & 0xFFFF, (key >> 16).to_le_bytes()).
  const std::array<uint8_t, 2> high{static_cast<uint8_t>(key >> 16),
                                    static_cast<uint8_t>(key >> 24)};
  uint16_t crc = crc16_update(static_cast<uint16_t>(key & 0xFFFF), high);
  std::vector<uint8_t> out;
  out.reserve(bytes.size());
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    crc = crc16_update(crc, std::span<const uint8_t>(&kMagic[i % kMagic.size()], 1));
    out.push_back(static_cast<uint8_t>(bytes[i] ^ (crc & 0xFF)));
  }
  return out;
}

Result<std::pair<std::string, std::string>> inquiry(ScsiTransport& transport) {
  std::array<uint8_t, 36> response{};
  const std::array<uint8_t, 6> command{0x12, 0, 0, 0, 36, 0};
  auto got = transport.in(command, response);
  if (!got) {
    return std::unexpected(got.error());
  }
  const auto text = [](std::span<const uint8_t> bytes) {
    std::string out;
    for (uint8_t byte : bytes) {
      if (byte == 0) break;
      out.push_back(static_cast<char>(byte));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) {
      out.pop_back();
    }
    return out;
  };
  return std::pair{text(std::span<const uint8_t>(response).subspan(8, 8)),
                   text(std::span<const uint8_t>(response).subspan(16, 16))};
}

Result<std::array<uint8_t, 14>> Loader::call(uint16_t command,
                                             std::span<const uint8_t> args) {
  const auto packet = cdb(command, args);
  std::array<uint8_t, 16> answer{};
  auto got = retry<std::size_t>(log_, [&] { return transport_.in(packet, answer); });
  if (!got) {
    return std::unexpected(got.error());
  }
  const uint16_t echoed = static_cast<uint16_t>((answer[0] << 8) | answer[1]);
  if (echoed != command) {
    return fail("the loader answered the wrong command");
  }
  std::array<uint8_t, 14> payload{};
  std::copy(answer.begin() + 2, answer.end(), payload.begin());
  return payload;
}

Result<void> Loader::upload(uint32_t address, std::span<const uint8_t> image) {
  std::size_t index = 0;
  for (std::size_t at = 0; at < image.size(); at += 512, ++index) {
    const auto block = image.subspan(at, std::min<std::size_t>(512, image.size() - at));
    const auto scrambled = cipher(block, 0xFFFF'FFFF);
    const auto args = sized_args(address + static_cast<uint32_t>(index * 512), scrambled);
    const auto packet = cdb(kWriteMemory, args);
    auto sent = retry<std::size_t>(log_, [&] { return transport_.out(packet, scrambled); });
    if (!sent) {
      return std::unexpected(sent.error());
    }
  }
  return {};
}

Result<void> Loader::jump(uint32_t address) {
  std::array<uint8_t, 6> args{};
  args[0] = static_cast<uint8_t>(address >> 24);
  args[1] = static_cast<uint8_t>(address >> 16);
  args[2] = static_cast<uint8_t>(address >> 8);
  args[3] = static_cast<uint8_t>(address);
  args[4] = 0;
  args[5] = 1;
  const auto packet = cdb(kJump, args);
  std::array<uint8_t, 16> answer{};
  auto got = retry<std::size_t>(log_, [&] { return transport_.in(packet, answer); });
  if (!got) {
    return std::unexpected(got.error());
  }
  return {};
}

Result<void> Loader::identify() {
  auto answer = call(kIdentify, {});
  if (!answer) {
    return std::unexpected(answer.error());
  }
  if (!std::equal(answer->begin(), answer->begin() + 4, "MFLR")) {
    return fail("the recovery loader did not answer");
  }
  return {};
}

Result<void> Loader::open(std::span<const uint8_t> loader_image) {
  auto identity = inquiry(transport_);
  if (!identity) {
    return std::unexpected(identity.error());
  }
  if (identity->first != "WL82" || identity->second != "UBOOT1.00") {
    return fail("not a WL82 UBOOT1.00 download-mode unit");
  }

  if (!identify()) {
    if (log_) log_("starting the recovery loader");
    auto uploaded = upload(kLoaderAddress, loader_image);
    if (!uploaded) return uploaded;
    auto jumped = jump(kLoaderAddress);
    if (!jumped) return jumped;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  auto ready = identify();
  if (!ready) {
    return ready;
  }

  const std::array<uint8_t, 4> key_arg{0x00, 0xAC, 0x69, 0x00};
  auto key_answer = call(kReadKey, key_arg);
  if (!key_answer) {
    return std::unexpected(key_answer.error());
  }
  const uint16_t raw_key = static_cast<uint16_t>(
      (*key_answer)[4] | (static_cast<uint16_t>((*key_answer)[5]) << 8));
  const std::array<uint8_t, 2> key_bytes{static_cast<uint8_t>(raw_key),
                                         static_cast<uint8_t>(raw_key >> 8)};
  const auto mixed = cipher(key_bytes, 0xFFFF'FFFF);
  chip_key_ = static_cast<uint16_t>(mixed[0] | (static_cast<uint16_t>(mixed[1]) << 8));

  auto online = call(kOnlineDevice, {});
  if (!online) {
    return std::unexpected(online.error());
  }
  flash_id_ = static_cast<uint32_t>((*online)[2]) |
              (static_cast<uint32_t>((*online)[3]) << 8) |
              (static_cast<uint32_t>((*online)[4]) << 16) |
              (static_cast<uint32_t>((*online)[5]) << 24);

  if (auto size_answer = call(kBufferSize, {}); size_answer) {
    const uint32_t size = (static_cast<uint32_t>((*size_answer)[0]) << 24) |
                          (static_cast<uint32_t>((*size_answer)[1]) << 16) |
                          (static_cast<uint32_t>((*size_answer)[2]) << 8) |
                          static_cast<uint32_t>((*size_answer)[3]);
    io_ = std::clamp<std::size_t>(size, 256, 4096);
  }

  if (log_) {
    log_("detected flash 0x" + std::to_string(flash_id_) + ", chip key 0x" +
         std::to_string(chip_key_));
  }
  return {};
}

Result<std::vector<uint8_t>> Loader::read(uint32_t address, std::size_t length) {
  std::vector<uint8_t> out(length);
  std::size_t done = 0;
  while (done < length) {
    const std::size_t chunk = std::min(io_, length - done);
    std::array<uint8_t, 6> args{};
    const uint32_t at = address + static_cast<uint32_t>(done);
    args[0] = static_cast<uint8_t>(at >> 24);
    args[1] = static_cast<uint8_t>(at >> 16);
    args[2] = static_cast<uint8_t>(at >> 8);
    args[3] = static_cast<uint8_t>(at);
    args[4] = static_cast<uint8_t>(chunk >> 8);
    args[5] = static_cast<uint8_t>(chunk);
    const auto packet = cdb(kReadFlash, args);
    auto got = retry<std::size_t>(log_, [&] {
      return transport_.in(packet, std::span<uint8_t>(out.data() + done, chunk));
    });
    if (!got) {
      return std::unexpected(got.error());
    }
    if (*got != chunk) {
      return fail("short flash read");
    }
    done += chunk;
  }
  return out;
}

Result<void> Loader::erase(uint32_t address, std::size_t length) {
  const uint32_t end = address + static_cast<uint32_t>(length);
  uint32_t at = address;
  while (at < end) {
    const bool block = (at % 0x10000 == 0) && (end - at >= 0x10000);
    const uint32_t size = block ? 0x10000 : 0x1000;
    const auto args = std::array<uint8_t, 4>{static_cast<uint8_t>(at >> 24),
                                             static_cast<uint8_t>(at >> 16),
                                             static_cast<uint8_t>(at >> 8),
                                             static_cast<uint8_t>(at)};
    auto result = call(block ? kEraseBlock : kEraseSector, args);
    if (!result) {
      return std::unexpected(result.error());
    }
    at += size;
  }
  return {};
}

Result<void> Loader::write(uint32_t address, std::span<const uint8_t> bytes) {
  for (std::size_t at = 0; at < bytes.size(); at += io_) {
    const auto chunk = bytes.subspan(at, std::min(io_, bytes.size() - at));
    if (std::all_of(chunk.begin(), chunk.end(), [](uint8_t byte) { return byte == 0xFF; })) {
      continue;
    }
    const auto args = sized_args(address + static_cast<uint32_t>(at), chunk);
    const auto packet = cdb(kWriteFlash, args);
    auto sent = retry<std::size_t>(log_, [&] { return transport_.out(packet, chunk); });
    if (!sent) {
      return std::unexpected(sent.error());
    }
  }
  return {};
}

Result<uint32_t> Loader::crc32_of(uint32_t address, std::size_t length) {
  std::array<uint8_t, 8> args{};
  args[0] = static_cast<uint8_t>(address >> 24);
  args[1] = static_cast<uint8_t>(address >> 16);
  args[2] = static_cast<uint8_t>(address >> 8);
  args[3] = static_cast<uint8_t>(address);
  args[4] = static_cast<uint8_t>(length >> 24);
  args[5] = static_cast<uint8_t>(length >> 16);
  args[6] = static_cast<uint8_t>(length >> 8);
  args[7] = static_cast<uint8_t>(length);
  auto answer = call(kCrc32, args);
  if (!answer) {
    return std::unexpected(answer.error());
  }
  return (static_cast<uint32_t>((*answer)[0]) << 24) |
         (static_cast<uint32_t>((*answer)[1]) << 16) |
         (static_cast<uint32_t>((*answer)[2]) << 8) |
         static_cast<uint32_t>((*answer)[3]);
}

}  // namespace fm1::wl82

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fm1/catalog.hpp"
#include "fm1/clock.hpp"
#include "fm1/flash_analysis.hpp"
#include "fm1/identity.hpp"
#include "fm1/midi.hpp"
#include "fm1/package.hpp"
#include "fm1/routing.hpp"
#include "fm1/update.hpp"
#include "fm1/wl82.hpp"
#include "fm1/ui/canvas.hpp"

static int failures = 0;

#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);            \
      ++failures;                                                                 \
    }                                                                             \
  } while (false)

#define CHECK_EQ(a, b)                                                            \
  do {                                                                            \
    const auto _a = (a);                                                          \
    const auto _b = (b);                                                          \
    if (!(_a == _b)) {                                                            \
      std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);          \
      ++failures;                                                                 \
    }                                                                             \
  } while (false)

namespace {

void test_midi_pack() {
  const std::array<uint8_t, 8> data{0x00, 0x59, 0x11, 0xAB, 0xFF, 0x01, 0x02, 0x03};
  const auto packed = fm1::midi::pack7(data);
  const auto unpacked = fm1::midi::unpack7(packed);
  CHECK_EQ(unpacked.size(), data.size());
  CHECK(std::equal(unpacked.begin(), unpacked.end(), data.begin()));
}

void test_identity() {
  // Build a handshake response: 00 59 11, then 27 bytes of text at 6..33.
  std::array<uint8_t, 34> body{};
  body[0] = 0;
  body[1] = 0x59;
  body[2] = 0x11;
  const char* text = "FM-1_916";
  std::memcpy(body.data() + 6, text, std::strlen(text));
  std::vector<uint8_t> sysex{0xF0};
  const auto packed = fm1::midi::pack7(body);
  sysex.insert(sysex.end(), packed.begin(), packed.end());
  sysex.push_back(0xF7);
  const auto identity = fm1::midi::parse_identity(sysex);
  CHECK(identity.has_value());
  CHECK_EQ(*identity, "FM-1_916");
  CHECK_EQ(fm1::identity::model_of(*identity), "FM-1");
  CHECK_EQ(fm1::identity::version_of(*identity), "916");
  CHECK(fm1::identity::package_fits("FM-1_916", "FM-1_900"));
  CHECK(!fm1::identity::package_fits("FM-1_916", "SMK-37 Pro_016"));
  CHECK(fm1::identity::same_identity("FM-1_91", "FM-1_916"));
}

fm1::midi::Packet packet(uint8_t cin, uint8_t a, uint8_t b = 0, uint8_t c = 0) {
  return fm1::midi::Packet{cin, a, b, c};
}

void test_sysex_assembler() {
  fm1::midi::SysexAssembler assembler;
  CHECK(!assembler.feed(packet(0x04, 0xF0, 0x22, 0x24)).has_value());
  const auto message = assembler.feed(packet(0x07, 0x35, 0x7F, 0xF7));
  CHECK(message.has_value());
  CHECK_EQ(message->size(), 6);
  CHECK_EQ((*message)[0], 0xF0);
  CHECK_EQ((*message)[3], 0x35);
  CHECK_EQ((*message)[5], 0xF7);
}

void test_request_and_response() {
  std::array<uint8_t, 15> body{};
  body[0] = 0;
  body[1] = 0x59;
  body[2] = 0x30;
  body[3] = 10;
  body[6] = 7;
  body[7] = 0x34;
  body[8] = 0x12;
  body[11] = 4;
  uint32_t sum = 0;
  for (std::size_t i = 6; i < 14; ++i) sum += body[i];
  body[14] = static_cast<uint8_t>(~sum);
  std::vector<uint8_t> sysex{0xF0};
  const auto packed = fm1::midi::pack7(body);
  sysex.insert(sysex.end(), packed.begin(), packed.end());
  sysex.push_back(0xF7);
  const auto request = fm1::midi::parse_request(sysex);
  CHECK(request.has_value());
  CHECK_EQ(request->flag, 7u);
  CHECK_EQ(request->address, 0x1234u);
  CHECK_EQ(request->length, 4u);

  const std::array<uint8_t, 2> data{0xAA, 0xBB};
  const auto response = fm1::midi::response(0x1234, data, 7);
  CHECK_EQ(response.front(), 0xF0);
  CHECK_EQ(response.back(), 0xF7);
}

struct MemoryReader : fm1::package::Reader {
  explicit MemoryReader(std::vector<uint8_t> data) : bytes(std::move(data)) {}
  std::vector<uint8_t> bytes;
  uint64_t size() const override { return bytes.size(); }
  fm1::Result<std::size_t> read_at(uint64_t offset, std::span<uint8_t> dst) override {
    if (offset + dst.size() > bytes.size()) return fm1::fail("short");
    std::memcpy(dst.data(), bytes.data() + offset, dst.size());
    return dst.size();
  }
};

std::vector<uint8_t> make_package(std::size_t blocks, std::string_view identity) {
  std::vector<uint8_t> fwsc((blocks + 1) * 0x30 + 64, 0xAA);
  for (std::size_t i = 0; i < blocks; ++i) {
    const uint8_t marker =
        i < identity.size() ? static_cast<uint8_t>(identity[i] + i + 1) : 0x7D;
    fwsc[i * 0x30 + 0x2F] = marker;
  }
  fwsc[blocks * 0x30 + 0x2F] = 0xFF;
  return fwsc;
}

void test_package() {
  const auto fwsc = make_package(20, "FM-1_916");
  const auto layout = fm1::package::inspect(fwsc);
  CHECK(layout.has_value());
  CHECK_EQ(layout->product, "FM-1_916");
  CHECK_EQ(layout->blocks, 20u);
  const auto image = fm1::package::logical_image(fwsc);
  CHECK_EQ(image.size(), fwsc.size() - 20);

  MemoryReader reader{fwsc};
  const auto file_layout = fm1::package::inspect(reader);
  CHECK(file_layout.has_value());
  fm1::package::LogicalImage logical(reader, *file_layout);
  std::vector<uint8_t> got(image.size());
  const auto result = logical.read(0, got);
  CHECK(result.has_value());
  CHECK(got == image);

  // Unaligned range crossing the interleave boundary.
  std::array<uint8_t, 100> part{};
  CHECK(logical.read(40, part).has_value());
  CHECK(std::equal(part.begin(), part.end(), image.begin() + 40));

  const auto smk = make_package(36, "SMK-37 Pro_016");
  const auto smk_layout = fm1::package::inspect(smk);
  CHECK(smk_layout.has_value());
  CHECK_EQ(smk_layout->product, "SMK-37 Pro_016");
  CHECK_EQ(smk_layout->blocks, 36u);
}

void test_update_serve() {
  const auto fwsc = make_package(20, "FM-1_916");
  MemoryReader reader{fwsc};
  const auto layout = fm1::package::inspect(reader);
  CHECK(layout.has_value());
  fm1::package::LogicalImage image(reader, *layout);

  std::vector<std::vector<uint8_t>> requests;
  const std::array<uint8_t, 15> request{0, 0x59, 0x30, 10, 0, 0, 7, 0, 0, 0, 0, 4, 0, 0, 0};
  auto body = request;
  uint32_t sum = 0;
  for (std::size_t i = 6; i < 14; ++i) sum += body[i];
  body[14] = static_cast<uint8_t>(~sum);
  std::vector<uint8_t> sysex{0xF0};
  const auto packed = fm1::midi::pack7(body);
  sysex.insert(sysex.end(), packed.begin(), packed.end());
  sysex.push_back(0xF7);
  requests.push_back(sysex);
  requests.push_back(std::vector<uint8_t>{0xF0, 0xF7});  // not a read request
  // FINISH_WRITE request.
  std::array<uint8_t, 15> finish{0, 0x59, 0x30, 10, 0, 0, 7, 0, 0, 0, 0xF0, 0, 0, 0, 0};
  uint32_t fsum = 0;
  for (std::size_t i = 6; i < 14; ++i) fsum += finish[i];
  finish[14] = static_cast<uint8_t>(~fsum);
  std::vector<uint8_t> finish_sysex{0xF0};
  const auto fpacked = fm1::midi::pack7(finish);
  finish_sysex.insert(finish_sysex.end(), fpacked.begin(), fpacked.end());
  finish_sysex.push_back(0xF7);
  requests.push_back(finish_sysex);

  std::size_t index = 0;
  std::vector<std::vector<uint8_t>> sent;
  const std::array<uint32_t, 1> finish_addresses{fm1::midi::kFinishWrite};
  const auto result = fm1::update::serve(
      image,
      [&](std::chrono::milliseconds) -> std::optional<std::vector<uint8_t>> {
        return index < requests.size() ? std::optional(requests[index++]) : std::nullopt;
      },
      [&](std::span<const uint8_t> sysex_out) {
        sent.emplace_back(sysex_out.begin(), sysex_out.end());
        return true;
      },
      [](std::size_t, std::size_t) {}, [](std::chrono::milliseconds) {},
      {.finish_addresses = finish_addresses,
       .idle_timeout = std::chrono::seconds(1),
       .pacing = std::chrono::milliseconds(0)});
  CHECK(result.has_value());
  CHECK_EQ(result->requests, 1u);
  CHECK_EQ(result->finish.value(), fm1::midi::kFinishWrite);
  CHECK_EQ(sent.size(), 2u);
}

void test_router_and_clock() {
  fm1::routing::Router router;
  CHECK(router.connect(0, 1));
  CHECK(router.forwards(0, 1));
  CHECK(!router.forwards(0, 2));
  router.toggle(0, 1);
  CHECK(!router.forwards(0, 1));
  router.toggle(0, 1);
  router.remove_port(1);
  CHECK(!router.forwards(0, 1));
  router.set_clock(2, true);
  CHECK(router.clock_enabled(2));
  CHECK_EQ(router.clock_targets(), static_cast<fm1::routing::Mask>(1u << 2));

  fm1::clock::Clock clock(120);
  CHECK_EQ(clock.interval_us(), 20'833u);
  clock.start(1000);
  CHECK_EQ(clock.next_due_us().value(), 1000u);
  clock.advance(1000);
  CHECK_EQ(clock.tick(), 1u);
  CHECK_EQ(clock.next_due_us().value(), 21'833u);
  clock.set_bpm(300);
  CHECK_EQ(clock.interval_us(), 8'333u);
  clock.stop();
  CHECK(!clock.next_due_us().has_value());
}

void test_wl82_cipher() {
  const std::array<uint8_t, 8> bytes{0, 1, 2, 3, 4, 5, 6, 7};
  const auto out = fm1::wl82::cipher(bytes, 0xFFFF'FFFF);
  const std::array<uint8_t, 8> expected{0x2F, 0xA5, 0x83, 0xCF, 0x4A, 0xC4, 0x5B, 0xED};
  CHECK(std::equal(out.begin(), out.end(), expected.begin()));
  CHECK_EQ(fm1::wl82::crc16(std::span<const uint8_t>(bytes)), 0x26B3u);
}

void test_flash_assets() {
  // Bundled m-bootloader files must parse and identify.
  const std::array<std::string, 2> paths{"data/bootloader-fm1.boot",
                                         "data/bootloader-smk37.boot"};
  for (const auto& path : paths) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) continue;
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 4096> buffer{};
    while (const auto got = std::fread(buffer.data(), 1, buffer.size(), file)) {
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(got));
    }
    std::fclose(file);
    const auto identity = fm1::flash::bootloader_identity(bytes);
    CHECK(identity.has_value());
    if (identity) {
      CHECK(identity->first == "fm1" || identity->first == "smk37");
    }
  }
}

void test_catalog() {
  const char* json = R"({
    "license": {"license": "CC-BY-4.0"},
    "firmwares": [
      {"id":"a","name":"Alpha","version":"1.0","author":"A","what":"first",
       "kind":["groove","dev"],"product":"FM-1_900","pkg":"fw/a.fwsc",
       "sha256":"aa","size":612200,"vintage":true,"site":"https://x",
       "fmhub":{"what":"nested","kind":"ignored","size":9}},
      {"id":"b","name":"Beta","version":"2.0","author":"B",
       "what":"second","kind":"drums","product":"SMK-37 Pro_016",
       "pkg":"fw/b.fwsc","size":5,"chip":"new"}
    ]
  })";
  std::vector<fm1::catalog::Entry> entries;
  fm1::catalog::StreamParser parser([&](fm1::catalog::Entry&& entry) {
    entries.push_back(std::move(entry));
  });
  parser.feed(std::span<const char>(json, std::strlen(json)));
  CHECK(parser.finish().has_value());
  CHECK_EQ(entries.size(), 2u);
  if (entries.size() == 2) {
    CHECK_EQ(entries[0].name, "Alpha");
    CHECK_EQ(entries[0].size, 612200u);
    CHECK(entries[0].vintage);
    CHECK(entries[0].tags.find("groove") != std::string::npos);
    CHECK(entries[0].tags.find("ignored") == std::string::npos);
    CHECK_EQ(entries[0].device, fm1::catalog::Device::Fm1);
    CHECK_EQ(entries[1].device, fm1::catalog::Device::Smk37);
    CHECK_EQ(entries[1].size, 5u);
    CHECK(fm1::catalog::works_on_new_chip(entries[1]));
  }

  // Cache round trip.
  const auto record = fm1::catalog::encode_record(entries[0]);
  const auto decoded = fm1::catalog::decode_record(record);
  CHECK(decoded.has_value());
  if (decoded) {
    CHECK_EQ(decoded->name, entries[0].name);
    CHECK_EQ(decoded->what, entries[0].what);
    CHECK_EQ(decoded->size, entries[0].size);
  }

  // Chunked strings: feed one byte at a time, including a long description.
  std::string long_json =
      R"({"firmwares":[{"name":"Long","what":")" + std::string(400, 'x') + R"(","kind":"x"}]})";
  std::vector<fm1::catalog::Entry> long_entries;
  fm1::catalog::StreamParser long_parser([&](fm1::catalog::Entry&& entry) {
    long_entries.push_back(std::move(entry));
  });
  for (char c : long_json) long_parser.feed(std::span<const char>(&c, 1));
  CHECK(long_parser.finish().has_value());
  CHECK_EQ(long_entries.size(), 1u);
  if (!long_entries.empty()) {
    CHECK_EQ(long_entries[0].name, "Long");
    CHECK_EQ(long_entries[0].what.size(), 240u);
  }
}

void test_canvas() {
  fm1::ui::Canvas canvas;
  canvas.clear(0);
  canvas.text(0, 0, "FM-1", 1);
  bool ink = false;
  for (int y = 0; y < 8 && !ink; ++y) {
    for (int x = 0; x < 30 && !ink; ++x) ink = canvas.at(x, y) != 0;
  }
  CHECK(ink);
  canvas.frame(10, 10, 20, 10, 3);
  CHECK_EQ(canvas.at(10, 10), 3);
  CHECK_EQ(canvas.at(29, 19), 3);
  CHECK_EQ(canvas.at(20, 14), 0);

  static std::size_t pixels = 0;
  static bool sizes_ok = true;
  pixels = 0;
  canvas.present(nullptr, [](void*, int, int, int width, int height,
                             std::span<const uint16_t> rgba) {
    pixels += rgba.size();
    if (static_cast<int>(rgba.size()) != width * height) sizes_ok = false;
  });
  CHECK(sizes_ok);
  CHECK_EQ(pixels, static_cast<std::size_t>(fm1::ui::Canvas::kWidth * fm1::ui::Canvas::kHeight));
}


void test_catalog_zoo() {
  const char* json = R"([
    {"name":"Zed","version":"1.2","byline":"maker","description":"a zoo entry",
     "device":"smk37pro","tags":["drums","new chip","vintage"],
     "url":"https://example.invalid/fw/zed.fwsc","website":"https://example.invalid",
     "source_url":"https://example.invalid/src"}
  ])";
  std::vector<fm1::catalog::Entry> entries;
  fm1::catalog::StreamParser parser([&](fm1::catalog::Entry&& entry) {
    entries.push_back(std::move(entry));
  });
  parser.feed(std::span<const char>(json, std::strlen(json)));
  CHECK(parser.finish().has_value());
  CHECK_EQ(entries.size(), 1u);
  if (!entries.empty()) {
    const auto& e = entries[0];
    CHECK_EQ(e.id, "Zed@1.2");
    CHECK_EQ(e.author, "maker");
    CHECK_EQ(e.what, "a zoo entry");
    CHECK_EQ(e.device, fm1::catalog::Device::Smk37);
    CHECK_EQ(e.pkg, "https://example.invalid/fw/zed.fwsc");
    CHECK_EQ(e.site, "https://example.invalid");
    CHECK_EQ(e.source, "https://example.invalid/src");
    CHECK(e.vintage);
    CHECK(fm1::catalog::works_on_new_chip(e));
    CHECK_EQ(fm1::catalog::package_url(e), e.pkg);
  }
}


void test_smk_variant() {
  fm1::catalog::Entry entry;
  entry.device = fm1::catalog::Device::Smk37;
  entry.tags = "synth SMK-37 Elite";
  CHECK_EQ(fm1::catalog::smk_variant_of(entry), fm1::catalog::SmkVariant::Elite);
  CHECK(fm1::catalog::matches_smk_variant(entry, fm1::catalog::SmkVariant::Elite));
  CHECK(!fm1::catalog::matches_smk_variant(entry, fm1::catalog::SmkVariant::Pro));
  entry.tags = "other SMK-37 Pro";
  CHECK_EQ(fm1::catalog::smk_variant_of(entry), fm1::catalog::SmkVariant::Pro);
  entry.tags = "other MKE-P37";
  CHECK_EQ(fm1::catalog::smk_variant_of(entry), fm1::catalog::SmkVariant::Mke);
  entry.tags = "other";
  CHECK_EQ(fm1::catalog::smk_variant_of(entry), fm1::catalog::SmkVariant::Pro);
  CHECK(fm1::catalog::matches_smk_variant(entry, fm1::catalog::SmkVariant::Pro));
  CHECK_EQ(fm1::catalog::smk_variant_from_product("SMK-37 Elite_016"),
           fm1::catalog::SmkVariant::Elite);
  CHECK_EQ(fm1::catalog::smk_variant_from_product("SMK-37 Pro_016"),
           fm1::catalog::SmkVariant::Pro);
  CHECK_EQ(fm1::catalog::smk_variant_from_product("MKE-P37_016"),
           fm1::catalog::SmkVariant::Mke);
}

}  // namespace

int main() {
  test_midi_pack();
  test_identity();
  test_sysex_assembler();
  test_request_and_response();
  test_package();
  test_update_serve();
  test_router_and_clock();
  test_wl82_cipher();
  test_flash_assets();
  test_catalog();
  test_catalog_zoo();
  test_smk_variant();
  test_canvas();
  if (failures == 0) {
    std::printf("all tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}

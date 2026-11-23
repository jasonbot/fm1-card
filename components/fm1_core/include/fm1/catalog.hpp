#pragma once

// Catalog entries and the streaming reader. The Firmware Hub catalog is
// ~700 KB; lwjson feeds it through this parser one byte at a time, and each
// completed entry is handed to a callback (the SD cache writer). No DOM.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lwjson/lwjson.h>

#include "fm1/result.hpp"

namespace fm1::catalog {

enum class Device : uint8_t { Fm1, Smk37 };

struct Entry {
  std::string id;
  std::string name;
  std::string version;
  std::string author;
  std::string what;
  std::string tags;  // space-separated: kind + extra tags
  std::string product;
  std::string pkg;
  std::string sha256;
  std::string site;
  std::string source;
  uint32_t size = 0;
  Device device = Device::Fm1;
  bool vintage = false;
};

Device device_from_product(std::string_view product);
bool matches(const Entry& entry, std::string_view needle);
bool works_on_new_chip(const Entry& entry);
bool works_on_old_chip(const Entry& entry);

// URL relative to the hub root.
std::string package_url(const Entry& entry);

// Length-prefixed record used by the SD cache.
std::vector<uint8_t> encode_record(const Entry& entry);
Result<Entry> decode_record(std::span<const uint8_t> bytes);

class StreamParser {
 public:
  using EntryFn = std::function<void(Entry&&)>;
  explicit StreamParser(EntryFn on_entry);
  StreamParser(const StreamParser&) = delete;
  StreamParser& operator=(const StreamParser&) = delete;
  ~StreamParser();

  void feed(std::span<const char> chunk);
  Result<void> finish();

  [[nodiscard]] uint32_t entries() const { return entries_; }

 private:
  static void trampoline(lwjson_stream_parser_t* parser, lwjson_stream_type_t type);
  void event(lwjson_stream_type_t type);
  void commit(std::string_view key);
  void append(std::string& target, std::size_t limit, std::string_view text);
  void start_entry();
  void end_entry();

  lwjson_stream_parser_t raw_{};
  EntryFn on_entry_;
  Entry current_;
  std::string field_;
  std::string array_;
  std::string key_;
  bool in_entry_ = false;
  bool root_array_ = false;
  bool device_explicit_ = false;
  std::size_t base_depth_ = 3;
  bool error_ = false;
  uint32_t entries_ = 0;
};

}  // namespace fm1::catalog

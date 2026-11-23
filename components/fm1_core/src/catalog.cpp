#include "fm1/catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace fm1::catalog {
namespace {

constexpr std::size_t kMaxId = 40;
constexpr std::size_t kMaxName = 80;
constexpr std::size_t kMaxVersion = 40;
constexpr std::size_t kMaxAuthor = 80;
constexpr std::size_t kMaxWhat = 240;
constexpr std::size_t kMaxTags = 96;
constexpr std::size_t kMaxProduct = 48;
constexpr std::size_t kMaxPkg = 128;
constexpr std::size_t kMaxHash = 80;
constexpr std::size_t kMaxUrl = 160;

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}
void put_u32(std::vector<uint8_t>& out, uint32_t value) {
  put_u16(out, static_cast<uint16_t>(value));
  put_u16(out, static_cast<uint16_t>(value >> 16));
}
void put_str(std::vector<uint8_t>& out, std::string_view text) {
  const auto length = static_cast<uint16_t>(std::min<std::size_t>(text.size(), 0xFFFF));
  put_u16(out, length);
  out.insert(out.end(), text.begin(), text.begin() + length);
}

struct Cursor {
  std::span<const uint8_t> bytes;
  std::size_t at = 0;

  bool take(std::size_t count) { return at + count <= bytes.size(); }
  uint16_t u16() {
    const uint16_t value =
        static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8));
    at += 2;
    return value;
  }
  uint32_t u32() {
    const uint32_t value = static_cast<uint32_t>(u16()) |
                           (static_cast<uint32_t>(u16()) << 16);
    return value;
  }
  std::string str() {
    const uint16_t length = u16();
    if (!take(length)) return {};
    std::string out(reinterpret_cast<const char*>(bytes.data() + at), length);
    at += length;
    return out;
  }
};

}  // namespace

Device device_from_product(std::string_view product) {
  return lower(product).starts_with("smk") ? Device::Smk37 : Device::Fm1;
}

bool matches(const Entry& entry, std::string_view needle) {
  if (needle.empty()) return true;
  const std::string haystack =
      lower(entry.name + " " + entry.version + " " + entry.tags + " " + entry.author + " " +
            entry.what + " " + entry.product);
  std::size_t at = 0;
  while (at < needle.size()) {
    while (at < needle.size() && std::isspace(static_cast<unsigned char>(needle[at]))) ++at;
    const std::size_t start = at;
    while (at < needle.size() && !std::isspace(static_cast<unsigned char>(needle[at]))) ++at;
    if (at > start && haystack.find(lower(needle.substr(start, at - start))) == std::string::npos) {
      return false;
    }
  }
  return true;
}

bool works_on_new_chip(const Entry& entry) {
  const auto tags = lower(entry.tags);
  return tags.find("new chip") != std::string::npos ||
         tags.find("any chip") != std::string::npos;
}

bool works_on_old_chip(const Entry& entry) {
  const auto tags = lower(entry.tags);
  const bool tagged = tags.find("new chip") != std::string::npos ||
                      tags.find("old chip") != std::string::npos ||
                      tags.find("any chip") != std::string::npos;
  // No chip tag means the stock/default (old) flash.
  return !tagged || tags.find("old chip") != std::string::npos ||
         tags.find("any chip") != std::string::npos;
}

SmkVariant smk_variant_of(const Entry& entry) {
  const auto tags = lower(entry.tags);
  if (tags.find("smk-37 elite") != std::string::npos) return SmkVariant::Elite;
  if (tags.find("mke-p37") != std::string::npos) return SmkVariant::Mke;
  // Untagged SMK entries are the default Pro build.
  return SmkVariant::Pro;
}

SmkVariant smk_variant_from_product(std::string_view product) {
  const auto text = lower(product);
  if (text.find("elite") != std::string::npos) return SmkVariant::Elite;
  if (text.find("mke-p37") != std::string::npos || text.find("mke") != std::string::npos)
    return SmkVariant::Mke;
  if (text.find("smk-37 pro") != std::string::npos || text.find("smk37 pro") != std::string::npos)
    return SmkVariant::Pro;
  // Bare SMK-37 products are the default Pro build.
  return text.find("smk") != std::string::npos ? SmkVariant::Pro : SmkVariant::All;
}

bool matches_smk_variant(const Entry& entry, SmkVariant variant) {
  return variant == SmkVariant::All || smk_variant_of(entry) == variant;
}

std::string package_url(const Entry& entry) {
  // The firmware-zoo feed publishes absolute package URLs. Relative paths are
  // resolved against the zoo root, not any of the older mirrors.
  if (entry.pkg.starts_with("http://") || entry.pkg.starts_with("https://")) {
    return entry.pkg;
  }
  return "https://www.jasonscheirer.com/firmware-zoo/" + entry.pkg;
}

std::vector<uint8_t> encode_record(const Entry& entry) {
  std::vector<uint8_t> out;
  out.reserve(512);
  out.insert(out.end(), {'F', 'C', 'A', 'T'});
  put_u16(out, 1);
  put_str(out, entry.id);
  put_str(out, entry.name);
  put_str(out, entry.version);
  put_str(out, entry.author);
  put_str(out, entry.what);
  put_str(out, entry.tags);
  put_str(out, entry.product);
  put_str(out, entry.pkg);
  put_str(out, entry.sha256);
  put_str(out, entry.site);
  put_str(out, entry.source);
  put_u32(out, entry.size);
  out.push_back(static_cast<uint8_t>(entry.device));
  out.push_back(entry.vintage ? 1 : 0);
  return out;
}

Result<Entry> decode_record(std::span<const uint8_t> bytes) {
  if (bytes.size() < 6 || !std::equal(bytes.begin(), bytes.begin() + 4, "FCAT")) {
    return fail("bad catalog record");
  }
  Cursor cursor{bytes, 4};
  const uint16_t version = cursor.u16();
  if (version != 1) return fail("unsupported catalog record version");
  Entry entry;
  entry.id = cursor.str();
  entry.name = cursor.str();
  entry.version = cursor.str();
  entry.author = cursor.str();
  entry.what = cursor.str();
  entry.tags = cursor.str();
  entry.product = cursor.str();
  entry.pkg = cursor.str();
  entry.sha256 = cursor.str();
  entry.site = cursor.str();
  entry.source = cursor.str();
  if (!cursor.take(6)) return fail("short catalog record");
  entry.size = cursor.u32();
  entry.device = static_cast<Device>(bytes[cursor.at++]);
  entry.vintage = bytes[cursor.at++] != 0;
  return entry;
}

StreamParser::StreamParser(EntryFn on_entry) : on_entry_(std::move(on_entry)) {
  lwjson_stream_init(&raw_, &StreamParser::trampoline);
  lwjson_stream_set_user_data(&raw_, this);
}

StreamParser::~StreamParser() = default;

void StreamParser::feed(std::span<const char> chunk) {
  if (error_) return;
  for (char byte : chunk) {
    const auto result = lwjson_stream_parse(&raw_, byte);
    const bool is_error =
        (result >= lwjsonERR && result <= lwjsonERRPAR) || result >= lwjsonERRNULL;
    if (is_error) {
      error_ = true;
      return;
    }
  }
}

Result<void> StreamParser::finish() {
  if (error_) return fail("catalog JSON is malformed");
  if (in_entry_) return fail("catalog JSON ended inside an entry");
  return {};
}

void StreamParser::trampoline(lwjson_stream_parser_t* parser, lwjson_stream_type_t type) {
  auto* self = static_cast<StreamParser*>(lwjson_stream_get_user_data(parser));
  if (self != nullptr) self->event(type);
}

void StreamParser::start_entry() {
  current_ = Entry{};
  field_.clear();
  array_.clear();
  key_.clear();
  device_explicit_ = false;
  in_entry_ = true;
}

void StreamParser::end_entry() {
  if (!in_entry_) return;
  in_entry_ = false;
  if (current_.id.empty()) {
    current_.id = current_.name;
    if (!current_.version.empty()) current_.id += "@" + current_.version;
  }
  if (!device_explicit_) {
    current_.device = device_from_product(current_.product);
  }
  if (current_.product.empty()) {
    current_.product = current_.device == Device::Smk37 ? "SMK-37" : "FM-1";
  }
  if (!current_.vintage &&
      current_.tags.find("vintage") != std::string::npos) {
    current_.vintage = true;
  }
  ++entries_;
  if (on_entry_) on_entry_(std::move(current_));
  current_ = Entry{};
}

void StreamParser::append(std::string& target, std::size_t limit, std::string_view text) {
  if (target.size() >= limit) return;
  const std::size_t room = limit - target.size();
  target.append(text.data(), std::min(room, text.size()));
}

void StreamParser::commit(std::string_view key) {
  if (key == "id") current_.id = std::move(field_);
  else if (key == "name") current_.name = std::move(field_);
  else if (key == "version") current_.version = std::move(field_);
  else if (key == "author" || key == "byline") current_.author = std::move(field_);
  else if (key == "what" || key == "description") current_.what = std::move(field_);
  else if (key == "product") current_.product = std::move(field_);
  else if (key == "pkg" || key == "url") current_.pkg = std::move(field_);
  else if (key == "sha256") current_.sha256 = std::move(field_);
  else if (key == "site" || key == "website") current_.site = std::move(field_);
  else if (key == "source" || key == "source_url") current_.source = std::move(field_);
  else if (key == "device") {
    const auto lower_device = [&] {
      std::string text = field_;
      std::transform(text.begin(), text.end(), text.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return text;
    }();
    current_.device = lower_device.starts_with("smk") ? Device::Smk37 : Device::Fm1;
    device_explicit_ = true;
  } else if (key == "kind" || key == "tags") {
    if (!field_.empty()) {
      if (!current_.tags.empty()) current_.tags.push_back(' ');
      append(current_.tags, kMaxTags, field_);
    }
  } else if (key == "chip") {
    if (field_ == "new") append(current_.tags, kMaxTags, " new chip");
    else if (field_ == "any") append(current_.tags, kMaxTags, " any chip");
    else if (field_ == "old") append(current_.tags, kMaxTags, " old chip");
  }
  field_.clear();
}

void StreamParser::event(lwjson_stream_type_t type) {
  const auto& parser = raw_;
  const std::size_t pos = parser.stack_pos;
  const auto is_key = [&](std::size_t index) {
    return index < pos && parser.stack[index].type == LWJSON_STREAM_TYPE_KEY;
  };

  // The firmware-zoo catalog is a bare top-level array.
  if (type == LWJSON_STREAM_TYPE_ARRAY && pos == 0) {
    root_array_ = true;
    return;
  }
  if (!in_entry_ && root_array_) {
    if (type == LWJSON_STREAM_TYPE_OBJECT && pos == 1 &&
        parser.stack[0].type == LWJSON_STREAM_TYPE_ARRAY) {
      base_depth_ = 1;
      start_entry();
      return;
    }
    if (type == LWJSON_STREAM_TYPE_OBJECT_END && pos == 1 &&
        parser.stack[0].type == LWJSON_STREAM_TYPE_ARRAY) {
      end_entry();
      return;
    }
  }

  // Firmware-hub style: {"firmwares":[{...}, ...]}.
  if (!in_entry_ && type == LWJSON_STREAM_TYPE_OBJECT && pos == 3 &&
      parser.stack[2].type == LWJSON_STREAM_TYPE_ARRAY && is_key(1) &&
      std::strcmp(parser.stack[1].meta.name, "firmwares") == 0) {
    base_depth_ = 3;
    start_entry();
    return;
  }
  if (in_entry_ && type == LWJSON_STREAM_TYPE_OBJECT_END && pos == base_depth_ &&
      parser.stack[base_depth_ - 1].type == LWJSON_STREAM_TYPE_ARRAY) {
    end_entry();
    return;
  }
  if (!in_entry_) return;

  // A direct field key in the entry object.
  if (type == LWJSON_STREAM_TYPE_KEY && pos == base_depth_ + 1 &&
      parser.stack[base_depth_].type == LWJSON_STREAM_TYPE_OBJECT) {
    key_ = parser.data.str.buff;
    field_.clear();
    array_.clear();
    return;
  }

  // Direct scalar values.
  if (pos == base_depth_ + 2 && is_key(base_depth_ + 1)) {
    if (type == LWJSON_STREAM_TYPE_STRING || type == LWJSON_STREAM_TYPE_KEY) {
      const auto& str = parser.data.str;
      append(field_, kMaxWhat, std::string_view(str.buff, str.buff_pos));
      if (str.is_last) {
        commit(key_);
      }
      return;
    }
    if (type == LWJSON_STREAM_TYPE_NUMBER) {
      if (key_ == "size") {
        current_.size = static_cast<uint32_t>(
            std::strtoul(parser.data.prim.buff, nullptr, 10));
      }
      return;
    }
    if (type == LWJSON_STREAM_TYPE_TRUE || type == LWJSON_STREAM_TYPE_FALSE) {
      if (key_ == "vintage") current_.vintage = type == LWJSON_STREAM_TYPE_TRUE;
      return;
    }
  }

  // An array value directly under a field key (kind, tags, devices).
  if (pos == base_depth_ + 3 &&
      parser.stack[base_depth_ + 2].type == LWJSON_STREAM_TYPE_ARRAY &&
      is_key(base_depth_ + 1)) {
    if (type == LWJSON_STREAM_TYPE_STRING) {
      const auto& str = parser.data.str;
      append(field_, kMaxWhat, std::string_view(str.buff, str.buff_pos));
      if (str.is_last) {
        if (key_ == "kind" || key_ == "tags") {
          if (!current_.tags.empty()) current_.tags.push_back(' ');
          append(current_.tags, kMaxTags, field_);
        }
        field_.clear();
      }
    }
    return;
  }
}

}  // namespace fm1::catalog

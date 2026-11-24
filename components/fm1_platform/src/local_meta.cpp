#include "fm1/platform/library.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace fm1::platform {
namespace {

std::string meta_safe(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c == '\n' || c == '\r') c = ' ';
  }
  return out;
}


}  // namespace

LocalMetadata read_local_metadata(const std::string& package_path) {
  LocalMetadata meta;
  std::FILE* file = std::fopen((package_path + ".meta").c_str(), "rb");
  if (file == nullptr) return meta;
  char line[1024]{};
  auto read_line = [&](std::string& out) {
    if (std::fgets(line, sizeof(line), file) == nullptr) return false;
    out = line;
    const auto end = out.find_last_not_of("\r\n");
    out.resize(end == std::string::npos ? 0 : end + 1);
    return true;
  };
  std::string size_text;
  read_line(meta.source);
  read_line(meta.sha256);
  read_line(size_text);
  meta.size = std::strtoull(size_text.c_str(), nullptr, 10);
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    std::string text(line);
    const auto newline = text.find_last_not_of("\r\n");
    if (newline == std::string::npos) continue;
    text.resize(newline + 1);
    const auto eq = text.find('=');
    if (eq == std::string::npos) continue;
    const auto key = text.substr(0, eq);
    const auto value = text.substr(eq + 1);
    if (key == "id") meta.id = value;
    else if (key == "name") meta.name = value;
    else if (key == "version") meta.version = value;
    else if (key == "author") meta.author = value;
    else if (key == "what") meta.what = value;
    else if (key == "tags") meta.tags = value;
    else if (key == "product") meta.product = value;
  }
  std::fclose(file);
  struct stat info {};
  if (stat(package_path.c_str(), &info) != 0 || info.st_size <= 0) {
    meta.valid = false;
  } else {
    meta.valid = meta.size == 0 || static_cast<uint64_t>(info.st_size) == meta.size;
  }
  return meta;
}

Result<void> write_local_metadata(const std::string& package_path, std::string_view source,
                                  std::string_view sha256, std::size_t size,
                                  const catalog::Entry* entry) {
  const std::string path = package_path + ".meta";
  const std::string temp = path + ".tmp";
  std::FILE* file = std::fopen(temp.c_str(), "wb");
  if (file == nullptr) return fail("cannot write " + temp);
  std::fprintf(file, "%s\n%s\n%zu\n", std::string(source).c_str(),
               std::string(sha256).c_str(), size);
  if (entry != nullptr) {
    std::fprintf(file,
                 "id=%s\nname=%s\nversion=%s\nauthor=%s\nwhat=%s\ntags=%s\nproduct=%s\n",
                 meta_safe(entry->id).c_str(), meta_safe(entry->name).c_str(),
                 meta_safe(entry->version).c_str(), meta_safe(entry->author).c_str(),
                 meta_safe(entry->what).c_str(), meta_safe(entry->tags).c_str(),
                 meta_safe(entry->product).c_str());
  }
  std::fflush(file);
  std::fclose(file);
  std::remove(path.c_str());
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    std::remove(temp.c_str());
    return fail("cannot replace " + path);
  }
  return {};
}

}  // namespace fm1::platform

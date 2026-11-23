#include "fm1/platform/file.hpp"

#include <sys/stat.h>

namespace fm1::platform {

FileReader::~FileReader() { close(); }

Result<void> FileReader::open(const std::string& path) {
  close();
  file_ = std::fopen(path.c_str(), "rb");
  if (file_ == nullptr) return fail("cannot open " + path);
  struct stat info {};
  if (fstat(fileno(file_), &info) != 0) {
    close();
    return fail("cannot stat " + path);
  }
  size_ = static_cast<uint64_t>(info.st_size);
  return {};
}

uint64_t FileReader::size() const { return size_; }

Result<std::size_t> FileReader::read_at(uint64_t offset, std::span<uint8_t> dst) {
  if (file_ == nullptr) return fail("file is not open");
  if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
    return fail("seek failed");
  }
  const auto got = std::fread(dst.data(), 1, dst.size(), file_);
  if (got != dst.size()) return fail("short read");
  return got;
}

void FileReader::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  size_ = 0;
}

}  // namespace fm1::platform

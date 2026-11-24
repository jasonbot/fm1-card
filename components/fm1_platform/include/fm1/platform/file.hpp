#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

#include "fm1/package.hpp"
#include "fm1/result.hpp"

namespace fm1::platform {

class FileReader : public package::Reader {
 public:
  FileReader() = default;
  ~FileReader() override;
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  Result<void> open(const std::string& path);
  [[nodiscard]] uint64_t size() const override;
  Result<std::size_t> read_at(uint64_t offset, std::span<uint8_t> dst) override;
  void close();
  [[nodiscard]] bool is_open() const {
    return file_ != nullptr;
  }

 private:
  std::FILE* file_ = nullptr;
  uint64_t size_ = 0;
};

}  // namespace fm1::platform

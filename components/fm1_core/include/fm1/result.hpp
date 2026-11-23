#pragma once

#include <expected>
#include <string>

namespace fm1 {

struct Error {
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(std::string message) {
  return std::unexpected(Error{std::move(message)});
}

}  // namespace fm1

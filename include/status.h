#pragma once

#include <string>
#include <utility>

namespace csw {

enum class Code {
  kOk,
  kIoError,    // the operating system reported an error
  kPoisoned,   // an earlier fsync failed; the writer refuses all writes
  kNotFound,   // a file does not exist
  kCorrupt,    // a snapshot or record does not parse or fails its CRC
  kInvalidArgument,  // the request is refused before anything is written
};

struct Status {
  Code code = Code::kOk;
  std::string message;

  bool ok() const { return code == Code::kOk; }

  static Status Ok() { return Status{}; }
  static Status Error(Code c, std::string msg) { return Status{c, std::move(msg)}; }
};

// A value or the reason it is missing. Callers check status first.
template <typename T>
struct Result {
  Status status;
  T value{};
};

}  // namespace csw

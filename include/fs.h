#pragma once

// The only file interface the store uses. PosixFs runs it on a real
// directory. SimFs runs it in memory and records every operation, so the
// crash checker can replay any prefix of the trace.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "status.h"

namespace csw {

class File {
 public:
  virtual ~File() = default;
  // Appends at the current end of the file.
  virtual Status append(std::string_view bytes) = 0;
  // Makes all earlier appends and truncates durable.
  virtual Status sync() = 0;
  virtual Status truncate(uint64_t size) = 0;
  virtual uint64_t size() const = 0;
};

class FileSystem {
 public:
  virtual ~FileSystem() = default;
  // Opens for appending. Creates the file if it does not exist.
  virtual Result<std::unique_ptr<File>> open_append(const std::string& name) = 0;
  // Opens for writing from empty. Truncates an existing file.
  virtual Result<std::unique_ptr<File>> create_truncated(const std::string& name) = 0;
  virtual Result<std::string> read_all(const std::string& name) = 0;
  virtual bool exists(const std::string& name) = 0;
  // Replaces dst with src. Not durable until sync_dir succeeds.
  virtual Status rename(const std::string& src, const std::string& dst) = 0;
  virtual Status sync_dir() = 0;
};

}  // namespace csw

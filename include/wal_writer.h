#pragma once

// Owns the WAL file handle and knows the frame format. The store decides when
// to sync and when to cut; this class only writes whole frames.

#include <memory>

#include "fs.h"
#include "record.h"

namespace csw {

class WalWriter {
 public:
  explicit WalWriter(std::unique_ptr<File> file) : file_(std::move(file)) {}

  Status append(const Record& rec) { return file_->append(encode_frame(encode_record(rec))); }
  Status sync() { return file_->sync(); }
  Status cut(uint64_t size) { return file_->truncate(size); }
  uint64_t size() const { return file_->size(); }

 private:
  std::unique_ptr<File> file_;
};

}  // namespace csw

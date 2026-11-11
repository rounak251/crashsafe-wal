#pragma once

// Decorator that makes one chosen call of one kind fail. The failing call does
// not reach the wrapped file system. This tests the store's error paths only.
// It does not model what a failed fsync does to unsynced data; SimFs does that.

#include <cstdint>
#include <memory>
#include <string>

#include "fs.h"

namespace csw {

enum class FaultOp { kAppend, kSync, kRename, kSyncDir, kCount };

class FaultFs : public FileSystem {
 public:
  explicit FaultFs(FileSystem& inner) : inner_(inner) {}

  // The nth call (1-based) of op fails with kIoError. Zero means never.
  void fail_nth(FaultOp op, uint64_t nth) { fail_at_[index(op)] = nth; }
  uint64_t calls(FaultOp op) const { return calls_[index(op)]; }

  Result<std::unique_ptr<File>> open_append(const std::string& name) override;
  Result<std::unique_ptr<File>> create_truncated(const std::string& name) override;
  Result<std::string> read_all(const std::string& name) override;
  bool exists(const std::string& name) override;
  Status rename(const std::string& src, const std::string& dst) override;
  Status sync_dir() override;

  // Used by the file wrapper.
  Status before(FaultOp op);

 private:
  static size_t index(FaultOp op) { return static_cast<size_t>(op); }

  FileSystem& inner_;
  uint64_t calls_[static_cast<size_t>(FaultOp::kCount)] = {};
  uint64_t fail_at_[static_cast<size_t>(FaultOp::kCount)] = {};
};

}  // namespace csw

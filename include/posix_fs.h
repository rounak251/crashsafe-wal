#pragma once

#include <string>

#include "fs.h"

namespace csw {

// FileSystem on a real directory. Durability uses F_FULLFSYNC on macOS and
// fsync elsewhere.
class PosixFs : public FileSystem {
 public:
  explicit PosixFs(std::string dir);
  ~PosixFs() override;

  Result<std::unique_ptr<File>> open_append(const std::string& name) override;
  Result<std::unique_ptr<File>> create_truncated(const std::string& name) override;
  Result<std::string> read_all(const std::string& name) override;
  bool exists(const std::string& name) override;
  Status rename(const std::string& src, const std::string& dst) override;
  Status sync_dir() override;

  // Number of times F_FULLFSYNC failed and fsync was used instead.
  static uint64_t fullsync_fallbacks();

 private:
  std::string path(const std::string& name) const;
  std::string dir_;
};

}  // namespace csw

#include "posix_fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace csw {
namespace {

std::atomic<uint64_t> g_fullsync_fallbacks{0};

std::string errno_text(const char* what) {
  return std::string(what) + ": " + std::strerror(errno);
}

// The macOS fsync does not ask the drive to flush its cache. F_FULLFSYNC does.
// If the filesystem refuses F_FULLFSYNC we fall back to fsync and log it, so
// the difference is visible in the benchmark output.
int full_sync(int fd) {
#ifdef __APPLE__
  if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
  g_fullsync_fallbacks.fetch_add(1);
  std::fprintf(stderr, "F_FULLFSYNC failed (%s); using fsync\n", std::strerror(errno));
#endif
  return fsync(fd);
}

class PosixFile : public File {
 public:
  PosixFile(int fd) : fd_(fd) {}
  ~PosixFile() override { close(fd_); }

  Status append(std::string_view bytes) override {
    size_t done = 0;
    while (done < bytes.size()) {
      ssize_t n = write(fd_, bytes.data() + done, bytes.size() - done);
      if (n < 0) {
        if (errno == EINTR) continue;
        return Status::Error(Code::kIoError, errno_text("write"));
      }
      done += static_cast<size_t>(n);
    }
    return Status::Ok();
  }

  Status sync() override {
    if (full_sync(fd_) != 0) return Status::Error(Code::kIoError, errno_text("fsync"));
    return Status::Ok();
  }

  Status truncate(uint64_t new_size) override {
    if (ftruncate(fd_, static_cast<off_t>(new_size)) != 0) {
      return Status::Error(Code::kIoError, errno_text("ftruncate"));
    }
    return Status::Ok();
  }

  uint64_t size() const override {
    struct stat st {};
    if (fstat(fd_, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
  }

 private:
  int fd_;
};

}  // namespace

PosixFs::PosixFs(std::string dir) : dir_(std::move(dir)) {}
PosixFs::~PosixFs() = default;

std::string PosixFs::path(const std::string& name) const { return dir_ + "/" + name; }

uint64_t PosixFs::fullsync_fallbacks() { return g_fullsync_fallbacks.load(); }

Result<std::unique_ptr<File>> PosixFs::open_append(const std::string& name) {
  Result<std::unique_ptr<File>> r;
  int fd = open(path(name).c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) {
    r.status = Status::Error(Code::kIoError, errno_text("open"));
    return r;
  }
  r.value = std::make_unique<PosixFile>(fd);
  return r;
}

Result<std::unique_ptr<File>> PosixFs::create_truncated(const std::string& name) {
  Result<std::unique_ptr<File>> r;
  int fd = open(path(name).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    r.status = Status::Error(Code::kIoError, errno_text("open"));
    return r;
  }
  r.value = std::make_unique<PosixFile>(fd);
  return r;
}

Result<std::string> PosixFs::read_all(const std::string& name) {
  Result<std::string> r;
  int fd = open(path(name).c_str(), O_RDONLY);
  if (fd < 0) {
    r.status = Status::Error(errno == ENOENT ? Code::kNotFound : Code::kIoError,
                             errno_text("open"));
    return r;
  }
  char buf[65536];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      r.status = Status::Error(Code::kIoError, errno_text("read"));
      break;
    }
    if (n == 0) break;
    r.value.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  return r;
}

bool PosixFs::exists(const std::string& name) {
  struct stat st {};
  return stat(path(name).c_str(), &st) == 0;
}

Status PosixFs::rename(const std::string& src, const std::string& dst) {
  if (::rename(path(src).c_str(), path(dst).c_str()) != 0) {
    return Status::Error(Code::kIoError, errno_text("rename"));
  }
  return Status::Ok();
}

Status PosixFs::sync_dir() {
  int fd = open(dir_.c_str(), O_RDONLY);
  if (fd < 0) return Status::Error(Code::kIoError, errno_text("open dir"));
  int rc = full_sync(fd);
  close(fd);
  if (rc != 0) return Status::Error(Code::kIoError, errno_text("fsync dir"));
  return Status::Ok();
}

}  // namespace csw

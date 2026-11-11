#include "fault_fs.h"

namespace csw {
namespace {

class FaultFile : public File {
 public:
  FaultFile(FaultFs* owner, std::unique_ptr<File> inner) : owner_(owner), inner_(std::move(inner)) {}

  Status append(std::string_view bytes) override {
    Status st = owner_->before(FaultOp::kAppend);
    return st.ok() ? inner_->append(bytes) : st;
  }
  Status sync() override {
    Status st = owner_->before(FaultOp::kSync);
    return st.ok() ? inner_->sync() : st;
  }
  Status truncate(uint64_t size) override { return inner_->truncate(size); }
  uint64_t size() const override { return inner_->size(); }

 private:
  FaultFs* owner_;
  std::unique_ptr<File> inner_;
};

Status injected(const char* what) {
  return Status::Error(Code::kIoError, std::string("injected failure in ") + what);
}

}  // namespace

Status FaultFs::before(FaultOp op) {
  uint64_t n = ++calls_[index(op)];
  if (fail_at_[index(op)] == n) return injected("file operation");
  return Status::Ok();
}

Result<std::unique_ptr<File>> FaultFs::open_append(const std::string& name) {
  auto r = inner_.open_append(name);
  if (r.status.ok()) r.value = std::make_unique<FaultFile>(this, std::move(r.value));
  return r;
}

Result<std::unique_ptr<File>> FaultFs::create_truncated(const std::string& name) {
  auto r = inner_.create_truncated(name);
  if (r.status.ok()) r.value = std::make_unique<FaultFile>(this, std::move(r.value));
  return r;
}

Result<std::string> FaultFs::read_all(const std::string& name) { return inner_.read_all(name); }

bool FaultFs::exists(const std::string& name) { return inner_.exists(name); }

Status FaultFs::rename(const std::string& src, const std::string& dst) {
  Status st = before(FaultOp::kRename);
  return st.ok() ? inner_.rename(src, dst) : st;
}

Status FaultFs::sync_dir() {
  Status st = before(FaultOp::kSyncDir);
  return st.ok() ? inner_.sync_dir() : st;
}

}  // namespace csw

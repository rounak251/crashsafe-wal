#include "sim_fs.h"

namespace csw {

CrashModel replay_prefix(const std::vector<Event>& trace, size_t n) {
  CrashModel model;
  for (size_t i = 0; i < n && i < trace.size(); ++i) model.apply(trace[i]);
  return model;
}

namespace {

class SimFile : public File {
 public:
  SimFile(SimFs* fs, uint64_t inode) : fs_(fs), inode_(inode) {}

  Status append(std::string_view bytes) override {
    fs_->write(inode_, bytes);
    return Status::Ok();
  }
  Status sync() override { return fs_->sync_inode(inode_); }
  Status truncate(uint64_t size) override {
    fs_->truncate_inode(inode_, size);
    return Status::Ok();
  }
  uint64_t size() const override { return fs_->size_of(inode_); }

 private:
  SimFs* fs_;
  uint64_t inode_;
};

}  // namespace

void SimFs::emit(Event e) {
  model_.apply(e);
  if (tracing_) trace_.push_back(std::move(e));
}

void SimFs::mark(Event e) {
  if (tracing_) trace_.push_back(std::move(e));
}

uint64_t SimFs::create_name(const std::string& name) {
  uint64_t inode = next_inode_++;
  Event e;
  e.kind = EventKind::kCreate;
  e.name = name;
  e.inode = inode;
  emit(std::move(e));
  return inode;
}

void SimFs::load(const Image& image) {
  model_ = CrashModel();
  for (const auto& [name, content] : image) {
    model_.install(name, next_inode_++, content);
  }
}

Result<std::unique_ptr<File>> SimFs::open_append(const std::string& name) {
  Result<std::unique_ptr<File>> r;
  uint64_t inode = model_.live_has(name) ? model_.live_inode(name) : create_name(name);
  r.value = std::make_unique<SimFile>(this, inode);
  return r;
}

Result<std::unique_ptr<File>> SimFs::create_truncated(const std::string& name) {
  Result<std::unique_ptr<File>> r;
  uint64_t inode;
  if (model_.live_has(name)) {
    inode = model_.live_inode(name);
    truncate_inode(inode, 0);
  } else {
    inode = create_name(name);
  }
  r.value = std::make_unique<SimFile>(this, inode);
  return r;
}

Result<std::string> SimFs::read_all(const std::string& name) {
  Result<std::string> r;
  if (!model_.live_has(name)) {
    r.status = Status::Error(Code::kNotFound, name + " does not exist");
    return r;
  }
  r.value = model_.live_content(model_.live_inode(name));
  return r;
}

bool SimFs::exists(const std::string& name) { return model_.live_has(name); }

Status SimFs::rename(const std::string& src, const std::string& dst) {
  if (!model_.live_has(src)) return Status::Error(Code::kNotFound, src + " does not exist");
  Event e;
  e.kind = EventKind::kRename;
  e.name = src;
  e.dst = dst;
  emit(std::move(e));
  return Status::Ok();
}

Status SimFs::sync_dir() {
  Event e;
  e.kind = EventKind::kSyncDir;
  emit(std::move(e));
  return Status::Ok();
}

void SimFs::write(uint64_t inode, std::string_view bytes) {
  Event e;
  e.kind = EventKind::kWrite;
  e.inode = inode;
  e.offset = model_.live_size(inode);
  e.bytes.assign(bytes.data(), bytes.size());
  emit(std::move(e));
}

Status SimFs::sync_inode(uint64_t inode) {
  ++file_sync_calls_;
  bool ok = !(fault_at_ != 0 && file_sync_calls_ == fault_at_);
  if (!ok) fault_fired_ = true;
  Event e;
  e.kind = EventKind::kSync;
  e.inode = inode;
  e.ok = ok;
  emit(std::move(e));
  return ok ? Status::Ok() : Status::Error(Code::kIoError, "injected fsync failure");
}

void SimFs::truncate_inode(uint64_t inode, uint64_t size) {
  Event e;
  e.kind = EventKind::kTruncate;
  e.inode = inode;
  e.size = size;
  emit(std::move(e));
}

}  // namespace csw

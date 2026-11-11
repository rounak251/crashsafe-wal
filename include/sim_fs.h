#pragma once

// In-memory FileSystem. Every operation is applied to a CrashModel and, while
// tracing is on, appended to a trace that the crash checker replays.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crash_model.h"
#include "fs.h"

namespace csw {

// The model after the first n events of a trace, rebuilt from scratch. The
// checker builds the same model one event at a time; tests compare the two.
CrashModel replay_prefix(const std::vector<Event>& trace, size_t n);

class SimFs : public FileSystem {
 public:
  SimFs() = default;

  // Fails the nth File::sync call (1-based). Directory syncs are not counted.
  // The failed fsync drops that file's unsynced data.
  void set_sync_fault(uint64_t nth) { fault_at_ = nth; }
  bool fault_fired() const { return fault_fired_; }
  uint64_t file_sync_calls() const { return file_sync_calls_; }

  void set_tracing(bool on) { tracing_ = on; }
  const std::vector<Event>& trace() const { return trace_; }
  // Adds a marker (Issue or Ack) to the trace. Markers do not change files.
  void mark(Event e);

  // Replaces the contents with a crash image. Everything in the image is
  // durable, so the model starts with no window.
  void load(const Image& image);
  Image durable_image() const { return model_.durable(); }
  Image live_image() const { return model_.live(); }
  const CrashModel& model() const { return model_; }

  Result<std::unique_ptr<File>> open_append(const std::string& name) override;
  Result<std::unique_ptr<File>> create_truncated(const std::string& name) override;
  Result<std::string> read_all(const std::string& name) override;
  bool exists(const std::string& name) override;
  Status rename(const std::string& src, const std::string& dst) override;
  Status sync_dir() override;

  // Used by SimFile.
  void write(uint64_t inode, std::string_view bytes);
  Status sync_inode(uint64_t inode);
  void truncate_inode(uint64_t inode, uint64_t size);
  uint64_t size_of(uint64_t inode) const { return model_.live_size(inode); }

 private:
  void emit(Event e);
  uint64_t create_name(const std::string& name);

  CrashModel model_;
  std::vector<Event> trace_;
  bool tracing_ = true;
  uint64_t next_inode_ = 1;
  uint64_t file_sync_calls_ = 0;
  uint64_t fault_at_ = 0;
  bool fault_fired_ = false;
};

}  // namespace csw

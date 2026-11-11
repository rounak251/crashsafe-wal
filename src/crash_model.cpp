#include "crash_model.h"

#include <algorithm>

namespace csw {
namespace {

std::string content_or_empty(const std::map<uint64_t, std::string>& data, uint64_t inode) {
  auto it = data.find(inode);
  return it == data.end() ? std::string() : it->second;
}

bool is_data_op(EventKind k) { return k == EventKind::kWrite || k == EventKind::kTruncate; }

}  // namespace

uint64_t WindowOp::options() const {
  if (kind == EventKind::kWrite) return bytes.size() + 1;
  return 2;
}

void CrashModel::apply(const Event& e) {
  switch (e.kind) {
    case EventKind::kCreate:
      live_ns_[e.name] = e.inode;
      live_data_[e.inode].clear();
      window_.push_back(WindowOp{EventKind::kCreate, e.inode, 0, 0, e.name, "", ""});
      break;

    case EventKind::kWrite: {
      std::string& c = live_data_[e.inode];
      size_t need = e.offset + e.bytes.size();
      if (c.size() < need) c.resize(need, '\0');
      c.replace(e.offset, e.bytes.size(), e.bytes);
      window_.push_back(WindowOp{EventKind::kWrite, e.inode, e.offset, 0, "", "", e.bytes});
      break;
    }

    case EventKind::kTruncate:
      live_data_[e.inode].resize(e.size, '\0');
      window_.push_back(WindowOp{EventKind::kTruncate, e.inode, 0, e.size, "", "", ""});
      break;

    case EventKind::kRename: {
      // Always logged, even when the source is missing. Whether it has an
      // effect depends on the outcomes of earlier window operations.
      auto it = live_ns_.find(e.name);
      if (it != live_ns_.end()) {
        live_ns_[e.dst] = it->second;
        live_ns_.erase(e.name);
      }
      window_.push_back(WindowOp{EventKind::kRename, 0, 0, 0, e.name, e.dst, ""});
      break;
    }

    case EventKind::kSync:
      if (e.ok) {
        // Every pending operation on this inode is now durable.
        durable_data_[e.inode] = live_data_[e.inode];
      } else {
        // A failed fsync drops the unsynced data. A later fsync cannot bring it back.
        live_data_[e.inode] = content_or_empty(durable_data_, e.inode);
      }
      window_.erase(std::remove_if(window_.begin(), window_.end(),
                                   [&](const WindowOp& w) {
                                     return is_data_op(w.kind) && w.inode == e.inode;
                                   }),
                    window_.end());
      break;

    case EventKind::kSyncDir:
      if (e.ok) {
        durable_ns_ = live_ns_;
      } else {
        live_ns_ = durable_ns_;
      }
      window_.erase(std::remove_if(window_.begin(), window_.end(),
                                   [](const WindowOp& w) {
                                     return w.kind == EventKind::kCreate ||
                                            w.kind == EventKind::kRename;
                                   }),
                    window_.end());
      break;

    case EventKind::kIssue:
    case EventKind::kAck:
      break;
  }
}

void CrashModel::install(const std::string& name, uint64_t inode, const std::string& content) {
  live_ns_[name] = inode;
  durable_ns_[name] = inode;
  live_data_[inode] = content;
  durable_data_[inode] = content;
}

Image CrashModel::live() const {
  Image out;
  for (const auto& [name, inode] : live_ns_) out[name] = content_or_empty(live_data_, inode);
  return out;
}

Image CrashModel::durable() const {
  Image out;
  for (const auto& [name, inode] : durable_ns_) out[name] = content_or_empty(durable_data_, inode);
  return out;
}

Image CrashModel::image(const std::vector<uint64_t>& choice) const {
  std::map<std::string, uint64_t> ns = durable_ns_;
  std::map<uint64_t, std::string> data = durable_data_;

  for (size_t i = 0; i < window_.size(); ++i) {
    const WindowOp& w = window_[i];
    uint64_t c = choice[i];
    switch (w.kind) {
      case EventKind::kCreate:
        if (c) ns[w.name] = w.inode;
        break;
      case EventKind::kRename: {
        auto it = ns.find(w.name);
        if (c && it != ns.end()) {
          ns[w.dst] = it->second;
          ns.erase(w.name);
        }
        break;
      }
      case EventKind::kWrite: {
        // c bytes of the write survive. The rest of the write is simply absent.
        // The write keeps the offset it was issued at. A dropped write before
        // it leaves a zero hole, which the frame CRC rejects.
        if (c == 0) break;
        std::string& content = data[w.inode];
        size_t need = w.offset + c;
        if (content.size() < need) content.resize(need, '\0');
        content.replace(w.offset, c, w.bytes, 0, c);
        break;
      }
      case EventKind::kTruncate:
        if (c) data[w.inode].resize(w.size, '\0');
        break;
      default:
        break;
    }
  }

  Image out;
  for (const auto& [name, inode] : ns) out[name] = content_or_empty(data, inode);
  return out;
}

uint64_t CrashModel::live_size(uint64_t inode) const {
  auto it = live_data_.find(inode);
  return it == live_data_.end() ? 0 : it->second.size();
}

std::string CrashModel::live_content(uint64_t inode) const {
  return content_or_empty(live_data_, inode);
}

}  // namespace csw

// Prints the file-system trace of one seeded workload, one event per line,
// with the size of the unsynced window after each event. Use it to check
// docs/crash-model.md against a real run.
//
//   trace_dump [A|B] [--seed N] [--limit N]

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "crash_model.h"
#include "crashcheck.h"

namespace {

const char* kind_name(csw::EventKind k) {
  switch (k) {
    case csw::EventKind::kCreate:
      return "create";
    case csw::EventKind::kWrite:
      return "write";
    case csw::EventKind::kTruncate:
      return "truncate";
    case csw::EventKind::kRename:
      return "rename";
    case csw::EventKind::kSync:
      return "fsync";
    case csw::EventKind::kSyncDir:
      return "fsync-dir";
    case csw::EventKind::kIssue:
      return "issue";
    case csw::EventKind::kAck:
      return "ack";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  char name = 'A';
  csw::CheckOptions opts;
  size_t limit = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "A") == 0 || std::strcmp(argv[i], "B") == 0) {
      name = argv[i][0];
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      opts.seed = std::stoull(argv[++i]);
    } else if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
      limit = std::stoul(argv[++i]);
    } else {
      std::fprintf(stderr, "usage: trace_dump [A|B] [--seed N] [--limit N]\n");
      return 2;
    }
  }

  std::vector<csw::Event> trace = csw::record_trace(name, opts);
  csw::CrashModel model;
  size_t shown = 0;
  for (size_t i = 0; i < trace.size(); ++i) {
    const csw::Event& e = trace[i];
    if (e.kind == csw::EventKind::kIssue || e.kind == csw::EventKind::kAck) {
      std::printf("%6zu  %-9s seq=%llu\n", i, kind_name(e.kind),
                  static_cast<unsigned long long>(e.seq));
    } else {
      model.apply(e);
      std::printf("%6zu  %-9s inode=%llu%s window=%zu\n", i, kind_name(e.kind),
                  static_cast<unsigned long long>(e.inode),
                  (e.kind == csw::EventKind::kSync || e.kind == csw::EventKind::kSyncDir) && !e.ok
                      ? " FAILED"
                      : "",
                  model.window().size());
    }
    if (limit && ++shown >= limit) break;
  }
  std::printf("%zu events in trace %c\n", trace.size(), name);
  return 0;
}

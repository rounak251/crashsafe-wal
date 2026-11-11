#include "crashcheck.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "crash_model.h"
#include "sim_fs.h"
#include "store.h"

namespace csw {
namespace {

// splitmix64: a small seeded generator, so every run and every sample is reproducible.
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed) {}
  uint64_t next() {
    uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  uint64_t below(uint64_t n) { return next() % n; }

 private:
  uint64_t s_;
};

// Forwards the store's markers into the trace of the simulated file system.
class TraceObserver : public Observer {
 public:
  explicit TraceObserver(SimFs& fs) : fs_(fs) {}
  void issued(uint64_t seq, const Op& op) override {
    Event e;
    e.kind = EventKind::kIssue;
    e.seq = seq;
    e.op = op;
    fs_.mark(std::move(e));
  }
  void acked(uint64_t seq) override {
    Event e;
    e.kind = EventKind::kAck;
    e.seq = seq;
    fs_.mark(std::move(e));
  }

 private:
  SimFs& fs_;
};

std::vector<Op> random_ops(Rng& rng, int count) {
  std::vector<Op> ops;
  for (int i = 0; i < count; ++i) {
    std::string key = "key-" + std::to_string(rng.below(64));
    if (rng.below(5) == 0) {
      ops.push_back(Op::Del(key));
    } else {
      std::string value;
      uint64_t len = 1 + rng.below(24);
      for (uint64_t k = 0; k < len; ++k) value.push_back(static_cast<char>('a' + rng.below(26)));
      ops.push_back(Op::Put(key, value));
    }
  }
  return ops;
}

struct WorkloadPlan {
  int ops;
  int checkpoint_every;
  uint64_t fault_at;  // file-sync call to fail, 0 for none
  SyncPolicy policy;
};

// Checks what the store must do after an injected fsync failure. These checks
// run on the workload itself, not on crash states, so their violation IDs are
// "<trace>/fault".
std::optional<Violation> check_after_fault(Store& store, const SimFs& fs, const std::string& name,
                                           const Status& failed) {
  auto bad = [&](const char* kind, const std::string& detail) {
    return std::optional<Violation>(Violation{name + "/fault", kind, detail});
  };
  if (failed.ok()) return bad("fault-not-reported", "the commit that hit the failed fsync returned success");
  if (!store.poisoned()) return bad("fault-not-poisoned", "a failed fsync did not poison the writer");
  size_t before = fs.trace().size();
  Status st = store.put("after-fault", "x");
  if (st.code != Code::kPoisoned) return bad("poisoned-code-missing", "a put after the fault did not return kPoisoned");
  for (size_t i = before; i < fs.trace().size(); ++i) {
    if (fs.trace()[i].kind == EventKind::kWrite) return bad("write-after-poison", "a refused put reached the WAL");
  }
  return std::nullopt;
}

// Runs the store on fs. Batches of 1, 2, 4 or 8 ops share one group sync
// under the group policy. When the fault fires, the driver stops and checks
// the poisoned-writer rule.
std::optional<Violation> drive(SimFs& fs, const WorkloadPlan& plan, uint64_t seed,
                               const std::string& name) {
  fs.set_sync_fault(plan.fault_at);
  TraceObserver observer(fs);
  StoreOptions opts;
  opts.observer = &observer;
  opts.policy = plan.policy;
  auto opened = Store::open(fs, opts);
  if (!opened.status.ok()) throw std::runtime_error("workload could not open: " + opened.status.message);
  Store& store = *opened.value;

  Rng rng(seed);
  static const int kBatches[] = {1, 2, 4, 8};
  int done = 0;
  int since_checkpoint = 0;
  while (done < plan.ops) {
    int batch = std::min(kBatches[rng.below(4)], plan.ops - done);
    Status st = store.commit(random_ops(rng, batch));
    done += batch;
    since_checkpoint += batch;
    if (fs.fault_fired()) return check_after_fault(store, fs, name, st);
    if (!st.ok()) throw std::runtime_error("commit failed without a fault: " + st.message);
    if (since_checkpoint >= plan.checkpoint_every) {
      Status c = store.checkpoint();
      if (!c.ok()) throw std::runtime_error("checkpoint failed without a fault: " + c.message);
      since_checkpoint = 0;
    }
  }
  return std::nullopt;
}

Violation make_violation(const std::string& id, const char* kind, const std::string& detail) {
  return Violation{id, kind, detail};
}

// Puts K keys after recovery, optionally checkpoints, then crashes with every
// unsynced byte dropped and reopens. The state must be prefix j plus the K keys.
std::optional<Violation> continue_and_check(const Image& image, const std::string& id, uint64_t j,
                                            const KvMap& before, int puts, bool checkpoint) {
  SimFs fs;
  fs.set_tracing(false);
  fs.load(image);
  auto opened = Store::open(fs);
  if (!opened.status.ok()) return make_violation(id, "recovery-failed", opened.status.message);
  Store& store = *opened.value;

  KvMap expect = before;
  for (int i = 0; i < puts; ++i) {
    Op op = Op::Put("cont-" + std::to_string(i), "c" + std::to_string(j) + "-" + std::to_string(i));
    Status st = store.commit({op});
    if (!st.ok()) return make_violation(id, "continuation-write-failed", st.message);
    expect[op.key] = op.value;
  }
  if (checkpoint) {
    // A leftover complete snapshot.tmp from a crash before the rename is
    // overwritten here, so a checkpoint that appends to it is caught.
    Status cp = store.checkpoint();
    if (!cp.ok()) return make_violation(id, "continuation-write-failed", "checkpoint: " + cp.message);
  }

  // The second crash drops everything unsynced. The continuation has synced
  // all of its writes, so this is the strictest image still consistent with
  // what the store acknowledged.
  SimFs crashed;
  crashed.set_tracing(false);
  crashed.load(fs.durable_image());
  auto again = Store::open(crashed);
  if (!again.status.ok()) return make_violation(id, "continuation-lost", "reopen: " + again.status.message);
  uint64_t want = j + static_cast<uint64_t>(puts);
  if (again.value->last_seq() != want || again.value->contents() != expect) {
    return make_violation(id, "continuation-lost", " want=" + std::to_string(want) +
                                                       " got=" + std::to_string(again.value->last_seq()));
  }
  return std::nullopt;
}

// Recovers one crash image and checks the oracle, then the continuation checks.
// The log continuation does not checkpoint, so a torn tail left in the log is
// not hidden by a checkpoint that rewrites the state. The checkpoint
// continuation then runs on the same image.
std::optional<Violation> check_image(const Image& image, uint64_t acked, uint64_t issued,
                                     const std::vector<KvMap>& prefix, const std::string& id,
                                     int continuation_puts) {
  std::string nums = " acked=" + std::to_string(acked) + " issued=" + std::to_string(issued);

  SimFs fs;
  fs.set_tracing(false);
  fs.load(image);
  auto opened = Store::open(fs);
  if (!opened.status.ok()) {
    return make_violation(id, "recovery-failed", opened.status.message);
  }
  Store& store = *opened.value;
  uint64_t j = store.last_seq();
  std::string at = " recovered=" + std::to_string(j) + nums;

  if (j < acked) return make_violation(id, "acked-op-lost", at);
  if (j > issued) return make_violation(id, "recovered-unissued", at);
  if (store.contents() != prefix[j]) return make_violation(id, "non-prefix-state", at);

  if (auto bad = continue_and_check(image, id, j, prefix[j], continuation_puts, false)) return bad;
  if (continuation_puts > 0) {
    return continue_and_check(image, id, j, prefix[j], continuation_puts, true);
  }
  return std::nullopt;
}

uint64_t mix(uint64_t seed, char trace, uint64_t point) {
  return seed ^ (static_cast<uint64_t>(trace) << 56) ^ (point * 0x9E3779B97F4A7C15ull);
}

// The outcome count the model document gives for one window operation. The
// code's WindowOp::options() must agree with it; a disagreement is a checker bug.
uint64_t documented_outcomes(const WindowOp& w) {
  if (w.kind == EventKind::kWrite) return w.bytes.size() + 1;
  return 2;
}

// The window as text for the TSV: w<L> for a write of L bytes, t, c, r for the
// other kinds. scripts/recount.sh rebuilds the state count from this text.
std::string window_tokens(const std::vector<WindowOp>& window) {
  if (window.empty()) return "-";
  std::string out;
  for (const WindowOp& w : window) {
    if (!out.empty()) out += ' ';
    switch (w.kind) {
      case EventKind::kWrite:
        out += "w" + std::to_string(w.bytes.size());
        break;
      case EventKind::kTruncate:
        out += "t";
        break;
      case EventKind::kCreate:
        out += "c";
        break;
      case EventKind::kRename:
        out += "r";
        break;
      default:
        out += "?";
        break;
    }
  }
  return out;
}

// Walks the crash points of one trace. Each point gets every outcome
// assignment if its window is small, and seeded samples if it is large.
bool enumerate_trace(const std::string& name, const std::vector<Event>& events,
                     const CheckOptions& o, std::ostream* points, TraceReport* rep,
                     std::optional<Violation>* violation) {
  rep->name = name;
  rep->events = events.size();
  CrashModel model;
  std::vector<KvMap> prefix(1);  // prefix[j]: state after issued ops 1..j
  uint64_t acked = 0;
  uint64_t issued = 0;

  for (size_t c = 0; c <= events.size(); ++c) {
    if (c > 0) {
      const Event& e = events[c - 1];
      if (e.kind == EventKind::kIssue) {
        if (e.seq != prefix.size()) throw std::runtime_error("issue seq is not contiguous");
        KvMap next = prefix.back();
        if (e.op.kind == Op::Kind::kPut) {
          next[e.op.key] = e.op.value;
        } else {
          next.erase(e.op.key);
        }
        prefix.push_back(std::move(next));
        issued = e.seq;
      } else if (e.kind == EventKind::kAck) {
        acked = e.seq;
      } else {
        model.apply(e);
      }
    }

    const auto& window = model.window();
    size_t wsize = window.size();
    rep->crash_points++;
    rep->max_window = std::max<uint64_t>(rep->max_window, wsize);
    if (rep->window_histogram.size() <= wsize) rep->window_histogram.resize(wsize + 1, 0);
    rep->window_histogram[wsize]++;
    std::vector<uint64_t> radix(wsize);
    for (size_t i = 0; i < wsize; ++i) {
      if (window[i].options() != documented_outcomes(window[i])) {
        *violation = Violation{name + "/c" + std::to_string(c) + "/model", "model-outcome-count",
                               "WindowOp::options() disagrees with docs/crash-model.md"};
        return false;
      }
      radix[i] = window[i].options();
    }

    uint64_t product = 1;
    for (uint64_t r : radix) product *= r;
    uint64_t here = 0;
    std::string mode;
    if (wsize <= static_cast<size_t>(o.window_bound)) {
      mode = "exhaustive";
      rep->exhaustive_points++;
      for (uint64_t v = 0; v < product; ++v) {
        std::vector<uint64_t> choice(wsize);
        uint64_t rest = v;
        for (size_t i = 0; i < wsize; ++i) {
          choice[i] = rest % radix[i];
          rest /= radix[i];
        }
        std::string id = name + "/c" + std::to_string(c) + "/v" + std::to_string(v);
        auto bad = check_image(model.image(choice), acked, issued, prefix, id, o.continuation_puts);
        ++here;
        if (bad) {
          *violation = bad;
          break;
        }
      }
    } else {
      mode = "sampled";
      rep->sampled_points++;
      Rng rng(mix(o.seed, name[0], c));
      for (int s = 0; s < o.samples; ++s) {
        std::vector<uint64_t> choice(wsize);
        for (size_t i = 0; i < wsize; ++i) choice[i] = rng.below(radix[i]);
        std::string id = name + "/c" + std::to_string(c) + "/s" + std::to_string(s);
        auto bad = check_image(model.image(choice), acked, issued, prefix, id, o.continuation_puts);
        ++here;
        if (bad) {
          *violation = bad;
          break;
        }
      }
    }
    rep->states += here;
    if (points) {
      *points << name << '\t' << c << '\t' << wsize << '\t' << mode << '\t' << here << '\t'
              << window_tokens(window) << '\n';
    }
    if (*violation) return false;
  }
  rep->ops_issued = issued;
  rep->ops_acked = acked;
  return true;
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (char ch : s) {
    if (ch == '"' || ch == '\\') out.push_back('\\');
    out.push_back(ch == '\n' ? ' ' : ch);
  }
  return out;
}

struct TraceSpec {
  char name;
  WorkloadPlan plan;
  uint64_t seed;
};

// A: group commit, checkpoints, no fault. B: group commit, fault in the 5th
// sync. C: per-op commit, fault in the 5th sync.
TraceSpec spec_for(char name, const CheckOptions& o) {
  if (name == 'A') {
    return TraceSpec{'A', WorkloadPlan{o.trace_a_ops, o.checkpoint_every, 0, SyncPolicy::kGroup}, o.seed};
  }
  if (name == 'B') {
    return TraceSpec{'B', WorkloadPlan{o.trace_b_ops, 1000000, 5, SyncPolicy::kGroup}, o.seed + 1};
  }
  return TraceSpec{'C', WorkloadPlan{o.trace_c_ops, 1000000, 5, SyncPolicy::kPerOp}, o.seed + 2};
}

// Runs the workload for one trace on fs, which must be a fresh disk.
std::optional<Violation> run_trace(char name, const CheckOptions& o, SimFs& fs) {
  TraceSpec spec = spec_for(name, o);
  return drive(fs, spec.plan, spec.seed, std::string(1, name));
}

}  // namespace

std::vector<Event> record_trace(char name, const CheckOptions& options) {
  SimFs fs;
  run_trace(name, options, fs);
  return fs.trace();
}

RunReport run_crashcheck(const CheckOptions& options, std::ostream* points) {
  auto start = std::chrono::steady_clock::now();
  RunReport report;
  report.options = options;

  for (char name : {'A', 'B', 'C'}) {
    SimFs fs;
    // The driver check runs first, but its violation is reported only after the
    // crash states of the same trace, which name the state where a bug shows.
    std::optional<Violation> driver_violation = run_trace(name, options, fs);
    TraceReport rep;
    rep.fault_fired = fs.fault_fired();
    std::optional<Violation> violation;
    enumerate_trace(std::string(1, name), fs.trace(), options, points, &rep, &violation);
    report.traces.push_back(rep);
    report.states_total += rep.states;
    if (violation) {
      report.violation = violation;
      break;
    }
    if (driver_violation) {
      report.violation = driver_violation;
      break;
    }
  }

  report.wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return report;
}

std::string report_to_json(const RunReport& r) {
  std::string j = "{\n";
  j += "  \"seed\": " + std::to_string(r.options.seed) + ",\n";
  j += "  \"window_bound\": " + std::to_string(r.options.window_bound) + ",\n";
  j += "  \"samples_per_sampled_point\": " + std::to_string(r.options.samples) + ",\n";
  j += "  \"continuation_puts\": " + std::to_string(r.options.continuation_puts) + ",\n";
  j += "  \"passed\": " + std::string(r.violation ? "false" : "true") + ",\n";
  if (r.violation) {
    j += "  \"violation\": {\"state\": \"" + json_escape(r.violation->state_id) + "\", \"kind\": \"" +
         json_escape(r.violation->kind) + "\", \"detail\": \"" + json_escape(r.violation->detail) +
         "\"},\n";
  } else {
    j += "  \"violation\": null,\n";
  }
  j += "  \"states_total\": " + std::to_string(r.states_total) + ",\n";
  j += "  \"wall_seconds\": " + std::to_string(r.wall_seconds) + ",\n";
  j += "  \"traces\": [\n";
  for (size_t i = 0; i < r.traces.size(); ++i) {
    const TraceReport& t = r.traces[i];
    j += "    {\"name\": \"" + t.name + "\", \"events\": " + std::to_string(t.events) +
         ", \"ops_issued\": " + std::to_string(t.ops_issued) +
         ", \"ops_acked\": " + std::to_string(t.ops_acked) +
         ", \"fault_fired\": " + std::string(t.fault_fired ? "true" : "false") +
         ", \"crash_points\": " + std::to_string(t.crash_points) +
         ", \"exhaustive_points\": " + std::to_string(t.exhaustive_points) +
         ", \"sampled_points\": " + std::to_string(t.sampled_points) +
         ", \"max_window\": " + std::to_string(t.max_window) +
         ", \"states\": " + std::to_string(t.states) + ", \"window_histogram\": [";
    for (size_t k = 0; k < t.window_histogram.size(); ++k) {
      j += (k ? ", " : "") + std::to_string(t.window_histogram[k]);
    }
    j += "]}";
    j += (i + 1 < r.traces.size()) ? ",\n" : "\n";
  }
  j += "  ]\n}\n";
  return j;
}

}  // namespace csw

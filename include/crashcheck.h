#pragma once

// Crash-state enumerator and oracle. See docs/crash-model.md for the model
// and the state-count formula that run_crashcheck implements.

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "crash_model.h"

namespace csw {

struct CheckOptions {
  uint64_t seed = 1;
  int window_bound = 2;  // W: windows up to this size are enumerated exhaustively
  int samples = 64;      // S: outcome assignments drawn for larger windows
  int trace_a_ops = 300;
  int trace_b_ops = 40;
  int trace_c_ops = 40;
  int checkpoint_every = 75;
  // K: puts after recovery, then a checkpoint, before the second crash. Zero
  // turns the continuation check off.
  int continuation_puts = 2;
};

struct Violation {
  std::string state_id;  // "<trace>/c<point>/v<index>" or ".../s<sample>"
  std::string kind;      // one of the names in docs/crash-model.md
  std::string detail;
};

struct TraceReport {
  std::string name;  // "A" or "B"
  uint64_t events = 0;
  uint64_t ops_issued = 0;
  uint64_t ops_acked = 0;
  bool fault_fired = false;
  uint64_t crash_points = 0;
  uint64_t exhaustive_points = 0;
  uint64_t sampled_points = 0;
  uint64_t max_window = 0;
  uint64_t states = 0;
  std::vector<uint64_t> window_histogram;  // crash points per window size
};

struct RunReport {
  CheckOptions options;
  std::vector<TraceReport> traces;
  uint64_t states_total = 0;
  std::optional<Violation> violation;  // the first violation; checking stops there
  double wall_seconds = 0;
};

// Runs traces A, B and C, enumerating every crash point. Writes one TSV row
// per crash point to points (may be null). Stops at the first violation.
RunReport run_crashcheck(const CheckOptions& options, std::ostream* points);

// The trace of one workload ('A' or 'B'), for inspection and the trace dump tool.
std::vector<Event> record_trace(char name, const CheckOptions& options);

// Serialises a report as JSON. Hand-written: the report has a fixed shape.
std::string report_to_json(const RunReport& report);

}  // namespace csw

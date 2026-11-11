#include <sstream>

#include "crashcheck.h"
#include "test.h"

using namespace csw;

TEST_CASE(small_crashcheck_run_passes_and_counts_consistently) {
  CheckOptions o;
  o.trace_a_ops = 40;
  o.trace_b_ops = 40;
  o.checkpoint_every = 20;
  o.samples = 8;
  std::ostringstream points;
  RunReport r = run_crashcheck(o, &points);
  CHECK(!r.violation.has_value());
  CHECK_EQ(r.traces.size(), 3u);
  CHECK(r.traces[1].fault_fired);
  CHECK(r.traces[2].fault_fired);
  uint64_t sum = 0;
  for (const auto& t : r.traces) sum += t.states;
  CHECK_EQ(sum, r.states_total);
  CHECK(r.states_total > 0);
  CHECK(r.traces[0].ops_acked == 40);
}

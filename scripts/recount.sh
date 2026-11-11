#!/bin/sh
# Recomputes the crash-state total from the formula in docs/crash-model.md.
# It rebuilds each row from that row's window (the "ops" column of the TSV)
# and the W and S values in the JSON report. It does not sum the enumerator's
# product or states, so an error in the enumerator's counting shows up here.
#   window size <= W: states = product over the window of outcomes
#                     (write of L bytes: L+1; truncate, create, rename: 2)
#   window size >  W: states = S
# Exits non-zero if a row's window, mode or states disagree with the formula,
# or if the total differs from the report.
set -eu
tsv=$1
json=$2
S=$(sed -n 's/.*"samples_per_sampled_point": \([0-9]*\).*/\1/p' "$json")
W=$(sed -n 's/.*"window_bound": \([0-9]*\).*/\1/p' "$json")
reported=$(sed -n 's/.*"states_total": \([0-9]*\).*/\1/p' "$json")
result=$(awk -F'\t' -v S="$S" -v W="$W" '
  NR == 1 { next }
  {
    n = 0
    prod = 1
    if ($6 != "-") {
      n = split($6, toks, " ")
      for (i = 1; i <= n; i++) {
        if (toks[i] ~ /^w[0-9]+$/) prod *= substr(toks[i], 2) + 1
        else if (toks[i] == "t" || toks[i] == "c" || toks[i] == "r") prod *= 2
        else { print "unknown window token " toks[i] > "/dev/stderr"; exit 1 }
      }
    }
    if (n <= W) { mode = "exhaustive"; states = prod } else { mode = "sampled"; states = S }
    if ($3 != n || $4 != mode || $5 != states) {
      bad++
      print "row disagrees with formula: " $0 > "/dev/stderr"
    }
    total += states
    rows++
  }
  END { printf "%.0f %d %d", total, rows, bad + 0 }' "$tsv")
set -- $result
recount=$1
rows=$2
bad=$3
echo "recount from formula: $recount (rows: $rows, rows disagreeing: $bad)"
echo "reported by crashcheck: $reported"
[ "$bad" = 0 ] && [ "$recount" = "$reported" ]

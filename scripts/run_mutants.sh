#!/bin/sh
# Builds the correct store (mutant 0) and each of the ten seeded bugs, runs
# crashcheck on each, and writes two tables:
#   bench/mutants.txt                  the full run (continuation check on)
#   bench/mutants-no-continuation.txt  the same run with --continuation-puts 0,
#                                      to show which bugs only the continuation check finds
# A seeded bug counts as CAUGHT only if crashcheck exits 1 and prints a
# VIOLATION line naming a crash state (or a "<trace>/fault" check). The
# correct store must PASS.
set -u
cd "$(dirname "$0")/.."
mkdir -p bench
status=0
names="0:baseline 1:no-dirsync-after-rename 2:no-dirsync-after-create 3:snapshot-in-place 4:snapshot-no-temp-fsync 5:crc-payload-only 6:no-frame-crc 7:torn-tail-not-truncated 8:ack-before-fsync 9:sync-error-ignored 10:sync-retry"

# run_table EXTRA_ARGS OUTFILE GATES
# GATES=1 makes a missed bug fail the script. The table without the continuation
# check is informational: it shows which bugs only the continuation check finds.
run_table() {
  extra=$1
  out=$2
  gates=$3
  table="$(printf '%-4s %-26s %-8s %s' ID NAME RESULT "VIOLATION (state, kind, detail)")
"
  for pair in $names; do
    id=${pair%%:*}
    name=${pair#*:}
    dir=build/mutant-$id
    mkdir -p "$dir"
    if ! make -s BUILD="$dir" EXTRA="-DMUTANT_ID=$id" "$dir/crashcheck" >"$dir/build.log" 2>&1; then
      table="$table$(printf '%-4s %-26s %-8s %s' "$id" "$name" BUILD-FAIL "see $dir/build.log")
"
      [ "$gates" = 1 ] && status=1
      continue
    fi
    # shellcheck disable=SC2086
    "$dir/crashcheck" $extra --json "$dir/crashcheck.json" --points "$dir/points.tsv" >"$dir/run.log" 2>&1
    rc=$?
    line=$(grep '^VIOLATION' "$dir/run.log" || true)
    if [ "$id" = 0 ]; then
      if [ $rc -eq 0 ]; then result=PASS; else result=FAIL; [ "$gates" = 1 ] && status=1; fi
    else
      if [ $rc -eq 1 ] && [ -n "$line" ]; then result=CAUGHT; else result=MISSED; [ "$gates" = 1 ] && status=1; fi
    fi
    table="$table$(printf '%-4s %-26s %-8s %s' "$id" "$name" "$result" "$line")
"
  done
  printf '%s' "$table" | tee "$out"
  echo
}

run_table "" bench/mutants.txt 1
echo "--- without the continuation check (--continuation-puts 0), informational ---"
run_table "--continuation-puts 0" bench/mutants-no-continuation.txt 0
exit $status

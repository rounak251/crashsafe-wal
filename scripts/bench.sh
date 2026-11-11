#!/bin/sh
# Runs the benchmark binary (two runs per invocation) and writes
# bench/results.json with the machine, the OS version and a source fingerprint.
# QUICK=1 selects the smoke mode. Smoke numbers must not be copied into the README.
set -eu
cd "$(dirname "$0")/.."
make -s BUILD=build/release build/release/bench

args="--runs 2"
quick=false
if [ "${QUICK:-0}" = 1 ]; then
  args="$args --quick"
  quick=true
fi

hw_model=$(sysctl -n hw.model 2>/dev/null || echo unknown)
hw_mem=$(sysctl -n hw.memsize 2>/dev/null || echo unknown)
sw_vers=$(sw_vers 2>/dev/null | tr '\t\n' '  ' | sed 's/  */ /g; s/ $//' || true)
[ -n "$sw_vers" ] || sw_vers=$(uname -sr)
uname_text=$(uname -sr)
sha=$(scripts/source_sha256.sh)
out=$(build/release/bench $args)

mkdir -p bench
{
  printf '{\n'
  printf '  "environment": {"hw_model": "%s", "hw_memsize_bytes": "%s", "sw_vers": "%s", "uname": "%s", "source_sha256": "%s", "quick": %s},\n' \
    "$hw_model" "$hw_mem" "$sw_vers" "$uname_text" "$sha" "$quick"
  printf '  "bench": %s\n' "$out"
  printf '}\n'
} > bench/results.json
echo "wrote bench/results.json"

#!/bin/sh
# One sha256 over the names and contents of the source and build inputs.
# This build has no git, so benchmark results record this instead of a commit SHA.
set -eu
cd "$(dirname "$0")/.."
if command -v shasum >/dev/null 2>&1; then SHA="shasum -a 256"; else SHA="sha256sum"; fi
files=$(find Makefile include src tools tests scripts -type f | sort)
for f in $files; do
  printf '%s\n' "$f"
  cat "$f"
done | $SHA | cut -d' ' -f1

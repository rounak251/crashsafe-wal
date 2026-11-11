#!/bin/sh
# Counts non-test C++ source lines with find and wc, so the count does not
# depend on cloc being installed. Tests and bench output are excluded, as in
# cloc --exclude-dir=tests,bench. "code" drops blank lines and whole-line
# // comments, which is the cloc definition for these files.
set -eu
cd "$(dirname "$0")/.."
files=$(find include src tools -type f \( -name '*.h' -o -name '*.cpp' \) | sort)
nfiles=$(echo "$files" | wc -l | tr -d ' ')
total=$(cat $files | wc -l | tr -d ' ')
code=$(cat $files | grep -v -E '^[[:space:]]*$' | grep -v -E '^[[:space:]]*//' | wc -l | tr -d ' ')
echo "files: $nfiles"
echo "total lines: $total"
echo "code lines: $code"

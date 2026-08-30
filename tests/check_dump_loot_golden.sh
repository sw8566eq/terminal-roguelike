#!/usr/bin/env bash
# Phase 4 of the testing overhaul (see CLAUDE.md's "Testing overhaul" section):
# automates what used to be a purely manual step in this project's own workflow — "for
# anything touching generation, gear or spawning, diff --seed=42 --dump-loot across
# floors 1-10 against the previous build" — into a real, ctest-registered regression
# check instead of something a human has to remember to run and eyeball.
#
# Covers map generation, depth gating, monster spawning, boss placement and carried gear
# (everything --dump-loot itself prints), by byte-comparing against a checked-in golden
# file rather than re-deriving what "correct" looks like.
#
# Usage: check_dump_loot_golden.sh <path-to-roguelike-binary> <path-to-golden-file>
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <path-to-roguelike-binary> <path-to-golden-file>" >&2
  exit 2
fi
BINARY="$1"
GOLDEN="$2"
ACTUAL="$(mktemp)"
trap 'rm -f "$ACTUAL"' EXIT

for floor in $(seq 1 10); do
  "$BINARY" --seed=42 --floor="$floor" --dump-loot
done > "$ACTUAL"

if ! diff -u "$GOLDEN" "$ACTUAL"; then
  echo "" >&2
  echo "FAIL: --seed=42 --dump-loot has drifted from the checked-in golden file ($GOLDEN)." >&2
  echo "If this drift is an intentional generation/content change, regenerate it:" >&2
  echo "  for f in \$(seq 1 10); do \"$BINARY\" --seed=42 --floor=\$f --dump-loot; done > $GOLDEN" >&2
  exit 1
fi
echo "OK: --seed=42 --dump-loot matches the golden file across floors 1-10."

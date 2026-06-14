#!/usr/bin/env bash
# Run the Lume test suite. A test passes if it exits 0 (asserts inside).
set -uo pipefail
cd "$(dirname "$0")/.."

LUME="${LUME:-build/lume}"
[ -x "$LUME" ] || { echo "Costruisci prima: ./scripts/build.sh"; exit 1; }

pass=0; fail=0
for t in tests/*.lm; do
  if out="$("$LUME" "$t" 2>&1)"; then
    echo "  ✓ $t"
    pass=$((pass+1))
  else
    echo "  ✗ $t"
    echo "$out" | sed 's/^/      /'
    fail=$((fail+1))
  fi
done

# Examples must also run without error (smoke test).
for e in examples/*.lm; do
  if out="$("$LUME" "$e" >/dev/null 2>&1)"; then :; else
    echo "  ✗ (example) $e"
    fail=$((fail+1))
  fi
done

# Error tests: these MUST fail cleanly (non-zero exit), never crash/segfault.
for t in tests/errors/*.lm; do
  [ -e "$t" ] || continue
  "$LUME" "$t" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -ge 64 ] && [ "$rc" -lt 128 ]; then
    echo "  ✓ (error) $t  (exit $rc)"
    pass=$((pass+1))
  else
    echo "  ✗ (error) $t  — atteso errore pulito, exit=$rc"
    fail=$((fail+1))
  fi
done

echo "-----------------------------------------"
echo "  passati: $pass   falliti: $fail"
[ "$fail" -eq 0 ]

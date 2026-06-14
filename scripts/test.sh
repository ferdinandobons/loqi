#!/usr/bin/env bash
# Run the Loqi test suite. A test passes if it exits 0 (asserts inside).
set -uo pipefail
cd "$(dirname "$0")/.."

LOQI="${LOQI:-build/loqi}"
[ -x "$LOQI" ] || { echo "Build first: ./scripts/build.sh"; exit 1; }

pass=0; fail=0
for t in tests/*.lq; do
  if out="$("$LOQI" "$t" 2>&1)"; then
    echo "  ✓ $t"
    pass=$((pass+1))
  else
    echo "  ✗ $t"
    echo "$out" | sed 's/^/      /'
    fail=$((fail+1))
  fi
done

# Examples must also run without error (smoke test).
for e in examples/*.lq; do
  if out="$("$LOQI" "$e" >/dev/null 2>&1)"; then :; else
    echo "  ✗ (example) $e"
    fail=$((fail+1))
  fi
done

# Error tests: these MUST fail cleanly (non-zero exit), never crash/segfault.
for t in tests/errors/*.lq; do
  [ -e "$t" ] || continue
  "$LOQI" "$t" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -ge 64 ] && [ "$rc" -lt 128 ]; then
    echo "  ✓ (error) $t  (exit $rc)"
    pass=$((pass+1))
  else
    echo "  ✗ (error) $t  — expected clean error exit, exit=$rc"
    fail=$((fail+1))
  fi
done

# Backtrace: an uncaught error must print the full call stack to stderr.
trace="$("$LOQI" tests/fixtures/backtrace_uncaught.lq 2>&1 1>/dev/null)"
if printf '%s' "$trace" | grep -q "at deepest" \
   && printf '%s' "$trace" | grep -q "at outer" \
   && printf '%s' "$trace" | grep -q "at <script>"; then
  echo "  ✓ (trace) uncaught-error backtrace shows the call stack"
  pass=$((pass+1))
else
  echo "  ✗ (trace) backtrace missing expected frames:"
  printf '%s\n' "$trace" | sed 's/^/      /'
  fail=$((fail+1))
fi

echo "-----------------------------------------"
echo "  passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]

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
    echo "  ✗ (error) $t, expected clean error exit, exit=$rc"
    fail=$((fail+1))
  fi
done

# Backtrace: an uncaught error must print the offending source line + the full
# call stack to stderr.
trace="$("$LOQI" tests/fixtures/backtrace_uncaught.lq 2>&1 1>/dev/null)"
if printf '%s' "$trace" | grep -q "at deepest" \
   && printf '%s' "$trace" | grep -q "at outer" \
   && printf '%s' "$trace" | grep -q "at <script>" \
   && printf '%s' "$trace" | grep -q "return \[\]\[3\]"; then
  echo "  ✓ (trace) uncaught error shows the source line + call stack"
  pass=$((pass+1))
else
  echo "  ✗ (trace) backtrace missing the source line or expected frames:"
  printf '%s\n' "$trace" | sed 's/^/      /'
  fail=$((fail+1))
fi

# ai() retry/backoff: point the endpoint at a closed local port so the connection
# fails (a transient error). With a dummy key, a tiny backoff and 2 retries, ai()
# must try exactly 3 times (1 + 2), then fail cleanly and fast, never hang. This
# exercises the retry loop fully offline.
retry_out="$(ANTHROPIC_API_KEY=dummy-key \
  LOQI_AI_BASE_URL='http://127.0.0.1:1/v1/messages' \
  LOQI_AI_MAX_RETRIES=2 LOQI_AI_RETRY_BASE_MS=1 \
  "$LOQI" tests/fixtures/ai_retry.lq 2>&1)"
rc=$?
if [ "$rc" -ge 64 ] && [ "$rc" -lt 128 ] && printf '%s' "$retry_out" | grep -q "after 3 attempts"; then
  echo "  ✓ (ai-retry) a transient failure is retried the configured number of times"
  pass=$((pass+1))
else
  echo "  ✗ (ai-retry) expected a bounded retry failure (exit 64-127, 'after 3 attempts'), exit=$rc:"
  printf '%s\n' "$retry_out" | sed 's/^/      /'
  fail=$((fail+1))
fi

# ai_all error path: same closed-port setup, fanned out. Each parallel call retries
# the transient failure then ai_all raises cleanly. This exercises the ai_all result
# loop, which must stay leak-clean (a parse error mid-loop must not strand the worker
# buffers or jobs array); run it under `leaks --atExit` locally to confirm 0.
allretry_out="$(ANTHROPIC_API_KEY=dummy-key \
  LOQI_AI_BASE_URL='http://127.0.0.1:1/v1/messages' \
  LOQI_AI_MAX_RETRIES=1 LOQI_AI_RETRY_BASE_MS=1 \
  "$LOQI" tests/fixtures/ai_all_retry.lq 2>&1)"
rc=$?
if [ "$rc" -ge 64 ] && [ "$rc" -lt 128 ] && printf '%s' "$allretry_out" | grep -q "after 2 attempts"; then
  echo "  ✓ (ai-all-retry) a fanned-out transient failure is retried and raised cleanly"
  pass=$((pass+1))
else
  echo "  ✗ (ai-all-retry) expected a bounded retry failure (exit 64-127, 'after 2 attempts'), exit=$rc:"
  printf '%s\n' "$allretry_out" | sed 's/^/      /'
  fail=$((fail+1))
fi

# Syntax-error caret: a parse error must print a file:line:column location, the
# offending source line, and a ^ caret under the exact column (here column 14).
caret_out="$("$LOQI" tests/fixtures/syntax_caret.lq 2>&1)"
rc=$?
if [ "$rc" -ge 64 ] && [ "$rc" -lt 128 ] \
   && printf '%s' "$caret_out" | grep -q ":4:14]" \
   && printf '%s' "$caret_out" | grep -q "let x = (1 + )" \
   && printf '%s' "$caret_out" | grep -q "\^"; then
  echo "  ✓ (caret) a syntax error shows line:column + the source line + a ^ caret"
  pass=$((pass+1))
else
  echo "  ✗ (caret) expected a line:column caret diagnostic, exit=$rc:"
  printf '%s\n' "$caret_out" | sed 's/^/      /'
  fail=$((fail+1))
fi

# Caret line attribution: an incomplete expression at end of line must report the
# line the code is ON (line 2), not the lexer's post-newline line. The rendered
# source line and the reported line:column must agree.
eol_out="$("$LOQI" tests/fixtures/syntax_caret_eol.lq 2>&1)"
rc=$?
if [ "$rc" -ge 64 ] && [ "$rc" -lt 128 ] \
   && printf '%s' "$eol_out" | grep -q ":5:12]" \
   && printf '%s' "$eol_out" | grep -q "5 | let b = 2 +"; then
  echo "  ✓ (caret-line) an end-of-line error is attributed to the right line"
  pass=$((pass+1))
else
  echo "  ✗ (caret-line) expected an error attributed to line 5 (:5:12 + 'let b = 2 +'), exit=$rc:"
  printf '%s\n' "$eol_out" | sed 's/^/      /'
  fail=$((fail+1))
fi

# http.serve: launch a tiny server on the loopback, hit it with curl, assert the
# string and map (status + content-type) responses, then stop it via /quit.
HTTP_PORT=8911
"$LOQI" tests/fixtures/http_server.lq >/dev/null 2>&1 &
HTTP_SRV=$!
http_up=0
for i in $(seq 1 50); do
  if curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/"; then http_up=1; break; fi
  sleep 0.1
done
if [ "$http_up" -eq 1 ]; then
  http_body="$(curl -s "http://127.0.0.1:$HTTP_PORT/ping")"
  http_json="$(curl -s -i "http://127.0.0.1:$HTTP_PORT/json")"
  if printf '%s' "$http_body" | grep -q "loqi-http GET /ping" \
     && printf '%s' "$http_json" | grep -q "201 Created" \
     && printf '%s' "$http_json" | grep -q "application/json"; then
    echo "  ✓ (http-serve) the built-in server answers string and map responses"
    pass=$((pass+1))
  else
    echo "  ✗ (http-serve) unexpected server responses:"
    printf '%s\n---\n%s\n' "$http_body" "$http_json" | sed 's/^/      /'
    fail=$((fail+1))
  fi
else
  echo "  ✗ (http-serve) server did not come up on port $HTTP_PORT"
  fail=$((fail+1))
fi
curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/quit" 2>/dev/null
kill "$HTTP_SRV" 2>/dev/null
wait "$HTTP_SRV" 2>/dev/null

echo "-----------------------------------------"
echo "  passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]

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

# ai usage reporting: a stubbed `curl` on PATH returns a fixed body with a usage
# block; ai(.., { usage: true }) must return { output, usage } with the token counts.
ai_stub="$(mktemp -d)"
cat > "$ai_stub/curl" <<'STUB'
#!/bin/sh
printf '%s' '{"content":[{"type":"text","text":"ok"}],"usage":{"input_tokens":11,"output_tokens":2}}'
printf '\n__LQHTTP:200__'
STUB
chmod +x "$ai_stub/curl"
usage_out="$(PATH="$ai_stub:$PATH" ANTHROPIC_API_KEY=dummy "$LOQI" tests/fixtures/ai_usage.lq 2>&1)"
rm -rf "$ai_stub"
if [ "$usage_out" = "ok|11|2" ]; then
  echo "  ✓ (ai-usage) ai(.., { usage: true }) returns { output, usage } with token counts"
  pass=$((pass+1))
else
  echo "  ✗ (ai-usage) expected 'ok|11|2', got: $usage_out"
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
   && printf '%s' "$eol_out" | grep -q ":6:11]" \
   && printf '%s' "$eol_out" | grep -q "6 | let b = 2 3"; then
  echo "  ✓ (caret-line) a syntax error is attributed to the right line"
  pass=$((pass+1))
else
  echo "  ✗ (caret-line) expected an error attributed to line 6 (:6:11 + 'let b = 2 3'), exit=$rc:"
  printf '%s\n' "$eol_out" | sed 's/^/      /'
  fail=$((fail+1))
fi

# Line continuation must be iterative, not recursive: a huge run of blank lines after
# a trailing operator must not overflow the C stack (it once segfaulted).
cont_deep="$(awk 'BEGIN{printf "let x = 1 +"; for(i=0;i<200000;i++) printf "\n"; print "2"; print "print(x)"}' | "$LOQI" /dev/stdin 2>&1)"
if [ "$?" -eq 0 ] && [ "$cont_deep" = "3" ]; then
  echo "  ✓ (cont-deep) 200k continued blank lines lex iteratively (no stack overflow)"
  pass=$((pass+1))
else
  echo "  ✗ (cont-deep) expected '3' and a clean exit, got: $cont_deep"
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
  http_boom="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/boom")"
  http_after="$(curl -s "http://127.0.0.1:$HTTP_PORT/after")"   # server must still be alive
  if printf '%s' "$http_body" | grep -q "loqi-http GET /ping" \
     && printf '%s' "$http_json" | grep -q "201 Created" \
     && printf '%s' "$http_json" | grep -q "application/json" \
     && [ "$http_boom" = "500" ] \
     && printf '%s' "$http_after" | grep -q "loqi-http GET /after"; then
    echo "  ✓ (http-serve) string/map responses, and a handler error is a 500 the server survives"
    pass=$((pass+1))
  else
    echo "  ✗ (http-serve) unexpected server responses (boom=$http_boom):"
    printf '%s\n---\n%s\n---\n%s\n' "$http_body" "$http_json" "$http_after" | sed 's/^/      /'
    fail=$((fail+1))
  fi
else
  echo "  ✗ (http-serve) server did not come up on port $HTTP_PORT"
  fail=$((fail+1))
fi
curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT/quit" 2>/dev/null
kill "$HTTP_SRV" 2>/dev/null
wait "$HTTP_SRV" 2>/dev/null

# Runtime-error caret: an uncaught runtime error prints the offending source line
# and a ^ caret under the faulting subexpression (here the out-of-bounds index).
rtcaret_out="$("$LOQI" tests/fixtures/runtime_caret.lq 2>&1)"
rc=$?
if [ "$rc" -ge 64 ] && [ "$rc" -lt 128 ] \
   && printf '%s' "$rtcaret_out" | grep -q "index 9 out of bounds" \
   && printf '%s' "$rtcaret_out" | grep -q "print(xs\[9\])" \
   && printf '%s' "$rtcaret_out" | grep -q "\^"; then
  echo "  ✓ (runtime-caret) a runtime error shows the source line + a ^ caret"
  pass=$((pass+1))
else
  echo "  ✗ (runtime-caret) expected a runtime error with a ^ caret, exit=$rc:"
  printf '%s\n' "$rtcaret_out" | sed 's/^/      /'
  fail=$((fail+1))
fi

echo "-----------------------------------------"
echo "  passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]

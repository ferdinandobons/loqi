# Loqi, Roadmap & Engineering Log

Loqi is built in a deliberate loop: **build → measure → decide the next
improvement → build again.** This document is the honest record of that loop.
Each iteration states what was done, what the measurements showed, and what they
imply for the next step.

## Feature status

| Area | Feature | Status |
|------|---------|--------|
| Core | lexer, parser, AST | ✅ done |
| Core | int/float/bool/nil/str/list/map/fn | ✅ done |
| Core | functions, closures, recursion | ✅ done |
| Core | control flow (if/while/for/break/continue) | ✅ done |
| Core | string interpolation (recursive) | ✅ done |
| Core | REPL | ✅ done |
| Stdlib | print, io, conversions, collections, strings, math | ✅ done |
| Speed | tree-walking interpreter | ✅ done (v0.1 baseline, retired) |
| Speed | **bytecode VM (stack, slot locals, upvalues)** | ✅ done |
| Speed | global hash table (O(1) lookup) | ✅ done |
| Speed | inline cache of global slots | ✅ done |
| Speed | computed-goto dispatch / NaN-boxing | 🔜 next |
| AI | `ai(...)` LLM call (Anthropic API) | ✅ done |
| AI | `json.parse` / `json.stringify` | ✅ done |
| AI | `http.get` / `http.post` | ✅ done |
| AI | `similarity` (cosine on vectors) | ✅ done |
| AI | `topk` (nearest-neighbour search) + `normalize`, RAG | ✅ done |
| AI | `env`, `read`, `write` | ✅ done |
| Lang | raw strings (backticks) | ✅ done |
| Lang | null-safety `??` / `?.`, ranges `a..b`, pipe `\|>` | ✅ done |
| Lang | arrow functions `x => e`, `(a, b) => e`, `() => e` | ✅ done |
| Lang | `if` & `match` as expressions (`let x = if c { a } else { b }`) | ✅ done |
| Lang | `const` immutable bindings | ✅ done |
| AI | `ai(prompt, options)` (model/system/temperature/max_tokens) | ✅ done |
| AI | structured JSON output (`ai(.., { json: true })`) | ✅ done |
| AI | `ai_json(prompt, schema)`, schema-validated output + retry | ✅ done |
| AI | `json.validate(value, schema)`, schema validation | ✅ done |
| AI | retry/backoff on 429/5xx + transient network errors | ✅ done |
| AI | `LOQI_AI_BASE_URL` endpoint override (gateway/proxy) | ✅ done |
| AI | `http.serve(port, handler)` (write an agent/API in Loqi) | ✅ done |
| AI | `embed` (embeddings API) | 🔜 next |
| AI | schema-constrained generation (`ai_json`) | ✅ done |
| Concurrency | `run_all(cmds)`, parallel subprocesses (thread pool) | ✅ done |
| Concurrency | `ai_all(prompts)`, concurrent model calls | ✅ done |
| Core | garbage collector (mark-sweep) | ✅ done |
| Core | namespaced modules (`import .. as`) + relative paths | ✅ done |
| Diagnostics | backtrace on uncaught errors | ✅ done |
| Stdlib | filesystem (ls/exists/mkdir/rm) + `path` ops | ✅ done |
| Stdlib | **regex** (linear-time Thompson NFA, ReDoS-proof) | ✅ done |
| Stdlib | date/time: `time.iso/parts/format/make/parse` (UTC) | ✅ done |
| Diagnostics | source-line snippet on uncaught errors | ✅ done |
| Diagnostics | accurate per-frame file names (cross-module) | ✅ done |
| Diagnostics | `^` caret + line:column on **syntax** errors | ✅ done |
| Diagnostics | `^` caret on **runtime** errors (needs per-op columns) | 🔜 next |
| Core | module cache (single-evaluation, diamond-safe) | ✅ done |
| Tooling | `loqi fmt`, `loqi test`, LSP | 📋 planned |
| Speed | AOT: emit C and compile (`loqi build`) | 🔬 exploring |

---

## Iteration 1, a correct, complete core (✅)

**Built.** A dependency-free reference interpreter in C11 (`src/loqi.c`):
lexer, Pratt parser, tree-walking evaluator, 22 native functions, REPL.
Compiles with nothing but `clang`. The whole language in `examples/02_basics.lq`
runs correctly: recursion, closures, higher-order functions, maps, list ops,
recursive string interpolation.

**Measured.** `fib(30)`, single thread, M-series, `process_time`:

| Engine | Time | vs Loqi |
|--------|------|---------|
| Loqi v0.1 (tree-walker) | ~0.55 s | 1.0× |
| CPython 3.13 | ~0.094 s | **5.8× faster** |
| Node 26 (V8 JIT) | ~0.017 s | **33× faster** |

**Decision.** This is the key finding: a language whose first promise is "fast"
is currently *slower than Python*. The tree-walker pays for it in two places
that profiling makes obvious:

1. **Variable lookup is linear + `strcmp`.** Resolving the name `fib` on every
   call scans the global scope (~23 entries) doing string compares. Over
   `fib(30)`'s ~2.7M calls that is tens of millions of `strcmp`.
2. **Allocation per call/loop/block.** Every function call `malloc`s a new
   environment and `strdup`s each parameter name.

Neither is fixable by micro-tuning the tree-walker, they are structural. The
canonical fix is a **bytecode VM** where locals are array slots resolved at
compile time and globals live in a hash table. That is iteration 2.

---

## Iteration 2, speed: bytecode VM (✅)

**Built.** Replaced the tree-walking engine with a stack-based **bytecode VM**,
reusing the lexer/parser/AST front-end unchanged. A single-pass compiler lowers
the AST to bytecode: locals resolve to stack slots at compile time, globals to an
open-addressing hash table, closures capture via upvalues. The whole test suite
and every example pass unchanged, same language, new engine.

**Measured.** Same machine, `process_time`:

| Workload | Loqi v0.1 (tree-walk) | Loqi v0.2 (VM) | CPython 3.13 | Node 26 (JIT) |
|----------|----------------------:|---------------:|-------------:|--------------:|
| `fib(30)` | 0.55 s | **0.105 s** | 0.094 s | 0.017 s |
| tight loop to 50M | n/a | **3.0 s** | 4.3 s | n/a |

**Result.** ~5× faster than the v0.1 tree-walker, and **on par with CPython** from
a VM written from scratch with no JIT. V8's JIT is still ahead on `fib`, closing
that gap is the next speed iteration.

**Re-measured (honest, v0.2 today, wall-clock best-of-3):**

| Workload | Loqi v0.2 | CPython 3.13 | Loqi vs CPython |
|----------|----------:|-------------:|-----------------|
| `fib(30)` | ~0.090 s | ~0.091 s | on par |
| tight loop 10M, **locals** | ~0.35 s | ~0.32 s | ~1.1× slower |
| tight loop 10M, **globals** | ~0.39 s | ~0.82 s | **~2× faster** |

After adding **inline caching of global slots** (each `OP_*_GLOBAL` caches its
resolved value slot, invalidated by a table version that bumps only on rehash),
global access went from ~5× slower than locals to roughly the same, a ~4.6×
speedup on global-heavy code, which also helps recursion (a call resolves the
function via a global lookup). Loqi is now on par with CPython on `fib` and ahead
on the loop. Next: computed-goto dispatch and NaN-boxed values.

**Decision.** The remaining headroom is in interpreter dispatch and global access:
(1) computed-goto dispatch instead of a `switch`, (2) caching global slots so a
`fib` call doesn't re-hash its name every time, (3) eventually NaN-boxed values.
These are iteration 4. First, iteration 3 ships the actual differentiator, the
AI-native surface, and the loop now also runs a continuous code-review pass.

---

## Iteration 3, the AI-first surface (✅)

The differentiator, now real. The things you install separately in other
languages are part of Loqi's runtime:

- `ai(prompt)` / `ai(prompt, model)` / `ai(prompt, options)`, a first-class LLM
  call (Anthropic Messages API). Reads `ANTHROPIC_API_KEY`; model defaults to
  `claude-sonnet-4-6` (override per-call or via `LOQI_AI_MODEL`). The options map
  takes `model`, `system`, `temperature`, `max_tokens`, and `json: true` to get
  parsed native data back (fence-tolerant).
- `json.parse` / `json.stringify`, a real JSON codec to/from native values.
- `http.get` / `http.post`, an HTTP client.
- `similarity(a, b)`, cosine similarity for semantic search.
- `env`, `read`, `write`, environment and files.
- **raw strings** (backticks) so JSON/regex literals need no escaping.

**Verified end-to-end:** `http.get` → `json.parse` fetches and reads live GitHub
data; `ai()` returns model text with a key and fails cleanly without one; all of
it is memory-clean under ASan/UBSan, including malformed-JSON and network paths.

**Implementation note (honest).** HTTP and the LLM call shell out to the
always-present `curl` (arguments are shell-quoted; the LLM request body goes
through a temp file, never the shell). A native HTTP client and an `embed()`
backed by an embeddings API are the next AI iteration.

---

## Iteration 2.5, the review loop hardens the VM (✅)

The build loop now includes a **continuous adversarial code review**. A multi-agent
pass (6 review dimensions → per-finding skeptical verification → synthesis) raised
29 findings; 28 were confirmed and all were fixed in one hardening pass:

- **Critical:** VM stack had no overflow guard, a large literal could write past
  `stack[]` and corrupt the heap. Now guarded; raises a clean error. (Reproduced
  by the reviewer, fixed, re-verified under ASan.)
- **High:** `snprintf` offset misuse; variadic-native arg/type checks; `import`
  swallowing errors; unbounded parser recursion; interpolation trailing-junk and
  the shadowable-`str` dependency.
- **Medium:** integer-overflow UB across arithmetic/negation/`INT64_MIN` division;
  `read_file` not validating directories / unseekable streams; control-byte
  corruption in strings; `break` array overflow.
- **Low/quality:** silent truncation limits, integer-literal range checks, CRLF
  continuation, dead-code removal, de-duplicated `value_to_cstr`.

The whole suite stays green under AddressSanitizer/UBSan. This pass runs every
iteration from now on.

## Iteration 4, completeness: learning from other languages (✅)

Renamed **Lume → Loqi** (the old name was already taken, an AI-first language, a
Rust language, and a CLI all shipped as "lume"; "Loqi" is verified free). Then
closed the biggest gaps that make other languages painful for real work:

- **`try` / `catch`**, the headline fix. Previously *any* error (bad JSON, a
  failed `http.get`, an out-of-bounds index) killed the whole program. Now risky
  code is recoverable; the error message binds to the catch variable. Catches both
  VM errors and built-in failures, across `map`/`filter` callbacks, with `return`
  from inside `try`. Implemented as a VM handler stack (no per-call C recursion).
- **`match`**, pattern matching over values, with `_` default and comma-OR arms.
- **A real standard library**, `sort`, `reverse`, `sum`, `min`/`max`, `slice`,
  `index_of`, `contains`, `del`, `map`/`filter`/`reduce`/`each`/`find`,
  `trim`/`replace`/`starts_with`/`ends_with`/`repeat`/`chars`, `round`/`ceil`/`pow`,
  `now`. Higher-order built-ins call back into the VM via a re-entrant `vm_invoke`.

All green under ASan/UBSan; regression tests in `tests/test_lang2.lq`.

## Backlog (📋)

Still ahead (the items above the line are done, GC, modules, schema-validated
output, regex, RAG, expression-form if/match, and the rest all shipped):

- `embed()` (embeddings API, needs a non-Anthropic provider/key).
- `ai` token counting / usage reporting (retry/backoff on 429/5xx already shipped).
- `loqi fmt`, `loqi test`, an LSP for editor support.
- Optional type annotations; block expressions that declare locals.
- Computed-goto dispatch and NaN-boxed values (raw VM speed).
- `loqi build`: AOT-compile a program to a native binary via emitted C.

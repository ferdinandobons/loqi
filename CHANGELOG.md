# Changelog

All notable changes to Loqi are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]


### Changed, product pivot: jq for the text only an LLM can parse
- **Repositioned around one job.** Loqi is now presented as a single static binary that
  turns messy text into schema-validated NDJSON in one pipe stage
  (`cat tickets.txt | loqi extract.lq > tickets.ndjson`), validated-or-fails. The
  README, landing page (`web/index.html`), `web/llms.txt`, `web/og.svg` and the docs now
  lead with the extraction job instead of "the AI-first programming language". The
  language reference (`docs/LANGUAGE.md`, `docs/STDLIB.md`, `docs/CHEATSHEET.md`,
  `docs/ROADMAP.md`) is unchanged in substance but reframed to lead with the job; RAG
  (`similarity`/`topk`/`normalize`), `http.serve` and modules are demoted to an
  "Also in the language" mention.
- **Perf reframed.** "Fast on Apple Silicon" is replaced everywhere by "instant startup,
  single static binary, macOS arm64 + Linux"; the honest fib(30)/loop benchmark is kept
  only in `docs/ROADMAP.md`.

### Added, the extraction pipeline (the product surface)
- **`stdin()` + `lines()`**, the pipe filter: read standard input and iterate it line by
  line (`for line in lines(stdin())`).
- **`ai_json(prompt, schema[, options])`**, schema-validated structured output with one
  automatic retry, the flagship call. `options` takes `model` and `temperature` (use
  `temperature: 0` for deterministic rows).
- **`ai_all(prompts)`**, parallel model calls returned in input order (batched, not
  streamed).
- **`ai_json_all(prompts, schema[, options])`**, the parallel **and** validated
  extractor: `ai_all`'s concurrency with `ai_json`'s validate-or-fails contract. Each
  row is schema-checked in input order with one per-row retry; a row that never
  validates fails the whole call naming the row index and the field. The fast path for
  turning many records into trustworthy typed rows.
- **`{ usage: true }`** on `ai`/`ai_all` for per-call token counts.
- **Schema constraints**: `type`, `enum`, `required`, `fields`, `items`, `pattern`,
  `min_length`/`max_length`, `min`/`max`.
- **Spend guard**: `LOQI_AI_DRY_RUN`, `LOQI_AI_MAX_CALLS`, and the CLI `--dry-run` /
  `--limit`, plus the documented `head -N | loqi extract.lq` idiom, so a fat-fingered
  `cat hugefile | loqi` can't quietly bill a fortune.
- **Linux support**: builds and CI green on macOS arm64 AND Linux (glibc), in addition
  to the existing memory-safety gates (ASan/UBSan/leaks clean). Runtime deps: `curl` and
  `ANTHROPIC_API_KEY`.
- **Container/CI correctness for the model call**: the key-bearing 0600 curl config file
  is now created under `$TMPDIR` (falling back to `/tmp` when unset), so Loqi behaves on
  hosts where `/tmp` is read-only or namespaced. A missing `curl` on `PATH` now fails the
  first `ai`/`ai_json`/`ai_all`/`http` call with a clear install hint
  (`apt install curl` / `apk add curl`) instead of a confusing "curl returned an error".

### Fixed
- **`sort()` on musl** (Alpine, and the upcoming static Linux binaries): the `qsort_r`
  portability shim keyed only on `__GLIBC__`, but musl uses the same GNU-style
  signature without defining it, so a musl build took the BSD branch and passed the
  context pointer where the comparator goes, crashing on the first `sort()`. The shim
  now keys on `__linux__` too. The glibc CI never hit this (it defines `__GLIBC__`);
  found while building the musl artifacts.

### Removed
- **`docs/WHY-LOQI.md`** (the old "AI-first programming language" manifesto, wrong
  audience) and **`docs/COMPARISON.md`** (general-language duel vs Go/Rust/Mojo). Their
  value moves into the README as a tight niche comparison table (vs jq / grep+awk /
  Python+SDK / `llm` / `mods`). All references in `README.md`, `web/llms.txt`,
  `CONTRIBUTING.md` and `CLAUDE.md` are updated to no longer point at them.

### Renamed
- **Lume → Loqi.** "Lume" was already taken (an AI-first language, a Rust language,
  and a `lume` CLI). After live availability research, the project is now **Loqi**
  (Latin *loqui*, "to speak"): no existing language, all package registries and the
  `loqi-lang` GitHub org free. Extension `.lm` → `.lq`, CLI `lume` → `loqi`.

### Added, language completeness (learning from other languages)
- **`try` / `catch`** error handling, recoverable errors (VM errors *and* built-in
  failures like `json.parse`/`http`/`ai`), with the message bound to the catch var.
  A program no longer dies at the first bad input or failed request.
- **`match`** pattern matching, `_` default, comma-OR patterns.
- **Standard library** filled out: `sort`, `reverse`, `sum`, `min`/`max`, `slice`,
  `index_of`, `contains`, `del`, `map`/`filter`/`reduce`/`each`/`find`,
  `trim`/`replace`/`starts_with`/`ends_with`/`repeat`/`chars`, `round`/`ceil`/`pow`,
  `now`. Higher-order built-ins via a re-entrant `vm_invoke`.
- Published to a public repository: https://github.com/ferdinandobons/loqi


### Changed
- **New execution engine: a stack-based bytecode VM** replaces the v0.1
  tree-walking interpreter. Locals are stack slots, globals live in an
  open-addressing hash table, closures capture via upvalues. ~5× faster than the
  tree-walker; on par with CPython on recursion, ~1.4× faster on tight loops.
  Same language surface, all tests and examples pass unchanged.

### Security & robustness (from the code-review loop)
A multi-agent adversarial review raised 29 findings (28 confirmed); all are fixed:
- **VM stack-overflow guard** in `vm_push`, a huge literal/expression now raises a
  clean runtime error instead of corrupting heap memory (was a critical OOB write).
- `snprintf` return value clamped in both error formatters (long paths could
  overflow the message buffer).
- Variadic natives (`push`, `range`, `assert`) validate argc and argument types
  (no more wild reads / reinterpreted bit patterns / unbounded allocation).
- `import` now propagates failures (missing file, syntax/runtime error) instead of
  swallowing them and exiting 0.
- Parser recursion-depth limit (pathological nesting → clean error, not SIGSEGV).
- Interpolation: trailing junk in `{...}` is now an error; a literal value is
  stringified via `+` coercion (no longer depends on a shadowable global `str`);
  unclosed `{` gives a precise message.
- Integer overflow (`+ - *`, negation, `INT64_MIN // -1`, `% -1`) handled without
  C undefined behavior; out-of-range integer/float literals rejected.
- `read_file` streams (works for pipes/stdin), rejects directories, checks errors.
- Compile-time limits: ≤255 args/params, ≤65535 list/map literal entries,
  ≤256 `break`s per loop, all diagnosed instead of silently truncating.
- CRLF line-continuation; control-byte rejection in strings; dead code removed;
  `value_to_cstr` repr de-duplicated.

### Added, AI-first batteries
- **`ai(prompt[, model])`**, a first-class LLM call (Anthropic Messages API via
  `curl`), reading `ANTHROPIC_API_KEY`; model defaults to `claude-sonnet-4-6`.
- **`json.parse` / `json.stringify`**, a built-in JSON codec (full string
  escapes incl. `\uXXXX`) to/from native values.
- **`http.get` / `http.post`**, a built-in HTTP client.
- **`similarity(a, b)`**, cosine similarity over numeric vectors.
- **`env`, `read`, `write`**, environment variables and whole-file I/O.
- **Raw strings** with backticks (`` `...` ``): verbatim, no escapes/interpolation,
  so JSON and regex literals need no escaping.
- **`ai` retry/backoff**: `ai` and `ai_all` now retry transient failures (HTTP 429,
  any 5xx, and transient network errors) with exponential backoff and jitter; a
  fatal 4xx is not retried. Tunable via `LOQI_AI_MAX_RETRIES` (default 3) and
  `LOQI_AI_RETRY_BASE_MS` (default 500). The endpoint is overridable with
  `LOQI_AI_BASE_URL` (a gateway/proxy, or a local mock for testing).
- **`ai` token usage.** `ai(prompt, { usage: true })` (and `ai_all`) returns
  `{ output, usage }` instead of bare text, carrying the API's `input_tokens` /
  `output_tokens` so a program can track tokens and cost per call.
- **`http.serve(port, handler)`**, a built-in single-threaded HTTP/1.1 server, so an
  agent/webhook/API is a Loqi program end to end (request → `ai()` → response). The
  handler is `fn(request)` returning a string or `{ status, body, content_type,
  headers }`. It runs on the main thread; an uncaught handler error becomes a `500`
  and the server keeps serving. Requests are bounds-checked (64 KB headers, 32 MB
  body). See `examples/ai/server.lq`.
- Example gallery `examples/ai/` (haiku, structured extraction, web+JSON).

### Added, ergonomics
- **Implicit line continuation.** A long expression continues onto the next line
  automatically when a line ends on a token a statement can't end on, a binary or
  logical operator, an open `(`/`[`, a trailing `,`, `=>`, `|>`, `??`, `?.`, or `.`.
  No trailing `\` needed; `price * qty +\n shipping` and multi-line calls/lists/maps
  and `|>` pipelines just work. (Fixes the stale "trailing `\`" claim in the docs.)

### Added, diagnostics
- **Carets on every error.** A parse error reports `file:line:column`, prints the
  offending source line, and underlines the exact spot with a `^` caret (Rust/Elm
  style). **Runtime errors now carry the same caret** under the faulting
  subexpression (e.g. the `[` of an out-of-bounds index, the `/` of a divide-by-zero),
  via per-op source-column tracking in the bytecode, on top of the existing source
  line + full backtrace.

### Fixed
- A `let` redeclaration of a module global after a `const` of the same name is now
  mutable again (`const x = 1; let x = 2; x = 3` was wrongly rejected at compile
  time, matching neither the runtime nor plain `let`/`let` redeclaration).

### In progress
- `embed()` (embeddings API).
- Presentation & positioning materials.

### Known limitations
- A `for` loop variable is one shared binding (Python-like); closures created in
  different iterations observe its final value. Per-iteration binding is planned.

## [0.1.0], 2026-06-14

First working version: a complete, correct core language.

### Added
- Lexer, Pratt parser and tree-walking evaluator (dependency-free C11).
- Types: `nil`, `bool`, `int` (i64), `float` (f64), `str`, `list`, `map`, `fn`.
- Functions, anonymous functions, closures, recursion.
- Control flow: `if`/`else if`/`else`, `while`, `for ... in`, `break`,
  `continue`, `return`.
- Recursive string interpolation with `{}` (handles nested strings & braces).
- `;` as an optional statement separator for one-liners.
- Structural equality for lists and maps.
- 22 built-in functions (I/O, conversions, collections, strings, math, time,
  testing), the "batteries included" core.
- REPL.
- Build script (`release` with `-O3 -mcpu=apple-m1 -flto`; `debug` with ASan/UBSan).
- Test suite + runner; runnable examples.
- Documentation: README, language guide, stdlib reference, roadmap.

# Changelog

All notable changes to Loqi are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

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
- Example gallery `examples/ai/` (haiku, structured extraction, web+JSON).

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

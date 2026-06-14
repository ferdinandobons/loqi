# Changelog

All notable changes to Lume are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Changed
- **New execution engine: a stack-based bytecode VM** replaces the v0.1
  tree-walking interpreter. Locals are stack slots, globals live in an
  open-addressing hash table, closures capture via upvalues. ~5× faster than the
  tree-walker; on par with CPython on recursion, ~1.4× faster on tight loops.
  Same language surface — all tests and examples pass unchanged.

### Security & robustness (from the code-review loop)
A multi-agent adversarial review raised 29 findings (28 confirmed); all are fixed:
- **VM stack-overflow guard** in `vm_push` — a huge literal/expression now raises a
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
  ≤256 `break`s per loop — all diagnosed instead of silently truncating.
- CRLF line-continuation; control-byte rejection in strings; dead code removed;
  `value_to_cstr` repr de-duplicated.

### In progress
- AI-native built-ins (`ai`, `embed`, `http`, `json`).
- Presentation & positioning materials.

### Known limitations
- A `for` loop variable is one shared binding (Python-like); closures created in
  different iterations observe its final value. Per-iteration binding is planned.

## [0.1.0] — 2026-06-14

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
  testing) — the "batteries included" core.
- REPL.
- Build script (`release` with `-O3 -mcpu=apple-m1 -flto`; `debug` with ASan/UBSan).
- Test suite + runner; runnable examples.
- Documentation: README, language guide, stdlib reference, roadmap.

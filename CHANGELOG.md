# Changelog

All notable changes to Lume are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### In progress
- Bytecode VM (arm64-tuned) to replace the tree-walking engine — see
  [ROADMAP](docs/ROADMAP.md).
- AI-native built-ins (`ai`, `embed`, `http`, `json`).

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

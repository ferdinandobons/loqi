# Lume — Roadmap & Engineering Log

Lume is built in a deliberate loop: **build → measure → decide the next
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
| Speed | tree-walking interpreter | ✅ done (baseline) |
| Speed | **bytecode VM (arm64-tuned)** | 🔜 in progress |
| Speed | resolver pass / slot-based locals | 🔜 in progress |
| AI | `ai "..."` LLM expression | 🔜 next |
| AI | structured generation with schema | 🔜 next |
| AI | `embed` / `similarity` (vectors) | 🔜 next |
| Batteries | `http`, `json`, `env`, `fs` | 🔜 next |
| Core | garbage collector (mark-sweep) | 📋 planned |
| Core | namespaced modules / packages | 📋 planned |
| Tooling | `lume fmt`, `lume test`, LSP | 📋 planned |
| Speed | AOT: emit C and compile (`lume build`) | 🔬 exploring |

---

## Iteration 1 — a correct, complete core (✅)

**Built.** A dependency-free reference interpreter in C11 (`src/lume.c`):
lexer, Pratt parser, tree-walking evaluator, 22 native functions, REPL.
Compiles with nothing but `clang`. The whole language in `examples/02_basics.lm`
runs correctly: recursion, closures, higher-order functions, maps, list ops,
recursive string interpolation.

**Measured.** `fib(30)`, single thread, M-series, `process_time`:

| Engine | Time | vs Lume |
|--------|------|---------|
| Lume v0.1 (tree-walker) | ~0.55 s | 1.0× |
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

Neither is fixable by micro-tuning the tree-walker — they are structural. The
canonical fix is a **bytecode VM** where locals are array slots resolved at
compile time and globals live in a hash table. That is iteration 2.

---

## Iteration 2 — speed: bytecode VM (🔜 in progress)

**Goal.** Beat CPython comfortably and get within a small factor of V8 on
compute-bound code, without giving up the dynamic, simple feel of the language.

**Plan.**
- Reuse the existing lexer/parser/AST as the front-end.
- Compile the AST to a compact bytecode (stack-based, `OP_*` instructions).
- Resolve locals to stack slots at compile time; globals to a hash table.
- Closures via upvalues.
- Dispatch via a computed-goto loop (`-fcomputed-goto`) on arm64.
- Keep `examples/` and the test suite green throughout — same language,
  faster engine.

**Success metric.** `fib(30)` faster than CPython by ≥3×; the full test suite
passes unchanged.

---

## Iteration 3 — the AI-first surface (🔜 next)

The differentiator. Make the things you install separately today part of the
language:

- `ai "prompt"` — an expression that calls an LLM and returns text. Uses
  `ANTHROPIC_API_KEY` from the environment; model selectable.
- `ai.json("prompt", schema)` — structured generation validated against a schema.
- `embed(text) → vector`, `similarity(a, b) → float` — semantic search built in.
- `http.get/post`, `json.parse/stringify`, `env.get`, `fs.read/write` — the
  plumbing every AI program needs, with zero installs.

These need network I/O. v0.1 of the AI layer shells out to the always-present
`curl`; a later iteration links a native HTTP client.

---

## Backlog (📋)

- Mark-sweep garbage collector (today: heap is freed at exit).
- Namespaced modules (`import math` → `math.sin`).
- `lume fmt`, `lume test`, an LSP for editor support.
- Pattern matching (`match`), optional type annotations.
- `lume build`: AOT-compile a program to a native binary via emitted C.

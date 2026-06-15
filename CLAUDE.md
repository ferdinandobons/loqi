# CLAUDE.md, working on Loqi

Guidance for Claude Code (and human contributors) hacking on **Loqi**, the AI-first
programming language. Read this before changing the implementation.

## What this is

A small, fast, AI-first language implemented in **dependency-free C11**. The entire
implementation is one file, **`src/loqi.c`** (~5.6k lines): lexer → Pratt parser →
single-pass bytecode compiler → stack VM → precise mark-and-sweep GC → native stdlib
(incl. the AI layer). Built with nothing but `clang`. File extension `.lq`, CLI `loqi`.

The **whole project is in English**, code, comments, error messages, examples, tests,
and filenames. (Conversations with the maintainer may be in Italian.)

## Build, run, test

```sh
./scripts/build.sh            # release: -O3 -mcpu=apple-m1 -flto -> build/loqi
./scripts/build.sh debug      # AddressSanitizer + UBSan
./scripts/test.sh             # full suite: tests/*.lq + examples + tests/errors/*.lq
./build/loqi path/to/file.lq  # run a program
./build/loqi                  # REPL
LOQI_GC_STRESS=1 ./build/loqi f.lq                          # collect on every instruction
MallocStackLogging=1 leaks --atExit -- ./build/loqi f.lq    # leak check (expect 0)
```

## Definition of done (non-negotiable)

Before committing any change to the implementation:

1. **Build + full suite + ASan/UBSan all green**, verify in a *separate step before*
   committing (a red test once slipped in via a compound build-and-commit command).
2. Add/extend tests in `tests/` (and an exit-code test in `tests/errors/` for new
   failure modes).
3. Keep the docs in sync (`docs/`, `README.md`, `docs/ROADMAP.md`), they are part of
   the surface; every claim should be backed by code and tests.
4. The interpreter must stay **leak-clean** (`leaks --atExit` → 0) and ASan/UBSan-clean.

The development style is a loop: **build → review (adversarial, ideally multi-agent) →
fix → repeat**. Memory-safety and correctness are verified empirically (ASan, UBSan,
`leaks`, `LOQI_GC_STRESS=1`), not assumed.

## Design decisions

- **Single file on purpose.** `src/loqi.c` is an amalgamation (à la SQLite): one
  compilation unit, trivially vendorable, zero dependencies. Don't split it without a
  strong reason; it's organized with clear `/* ===== section ===== */` headers.
- **Honest docs.** `docs/ROADMAP.md` tracks what's done vs planned with real (not
  aspirational) benchmarks. Don't claim features that aren't implemented and tested.

## Implementation gotchas (learned the hard way)

- **Locals are absolute frame-base slots** (`frame->slots[slot]`, `slot` fixed at
  compile time), not relative to the live stack top. A local declared *mid-expression*
  (temporaries already on the stack) gets the wrong slot. Hence: an `if`/`match` used
  as an **expression** may not declare locals in its branches (`compile_block_value`
  rejects it), and `match`-as-expression keeps its subject as a **stack temporary**
  tested with `OP_DUP`/`OP_EQUAL`/`OP_JUMP_IF_TRUE`, never a named local.
- **`ObjMap` is a dense array** (`entries[0..count)` valid; `[count..cap)` is garbage),
  iterate it by `->count`. The separate open-addressed `Table` type (globals,
  module_cache) iterates by `->cap` skipping NULL-key slots. Iterating an `ObjMap` by
  `->cap` reads uninitialized keys → crash.
- **GC runs only at the VM dispatch safe-point**, never inside a native. So building
  arena objects mid-native is safe and C-local Values needn't be rooted, but a native
  that `xmalloc`s a scratch buffer and then calls `runtime_error` (which `longjmp`s)
  **leaks** it. Validate inputs and raise *before* allocating scratch.
- **Compile errors** set `*had_error` and don't `longjmp`; compilation continues and the
  proto is discarded, so bytecode emitted after a compile error never runs.
- **Recursion is bounded** at compile time (`MAX_EXPR_DEPTH`, `MAX_STMT_DEPTH`,
  `MAX_FN_DEPTH`) and parse time (`MAX_PARSE_DEPTH`) so pathological nesting raises a
  clean error instead of overflowing the C stack. The AST free (`free_node`) is
  iterative for the same reason.
- **Git commit messages with backticks**: use `git commit -F <file>`, not `-m "...`...`"`
  (the shell runs the backticked text as a command and drops it from the message).

## Layout

```
src/loqi.c   the whole language (lexer, parser, bytecode compiler, VM, GC, stdlib)
scripts/     build.sh (release / debug=ASan+UBSan), test.sh
tests/       test_*.lq, errors/*.lq (exit-code tests), fixtures/
examples/    runnable .lq programs (examples/ai/ for AI demos, need a key)
docs/        LANGUAGE, STDLIB, CHEATSHEET, ROADMAP
web/         landing page + SEO assets (GitHub Pages)
editors/     TextMate grammar for syntax highlighting
```

## Security

`ai()` reads `ANTHROPIC_API_KEY` from the environment; the key only ever goes into a
0600 temp config file, never argv/argc. Never write keys to disk in the repo or commit
them.

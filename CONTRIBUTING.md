# Contributing to Lume

Thanks for your interest. Lume is built in a tight loop —
**build → measure → review → improve** — and contributions follow the same rhythm.

## Build & test

```sh
./scripts/build.sh          # release (-O3 -mcpu=apple-m1 -flto)
./scripts/build.sh debug    # ASan + UBSan, for catching memory bugs
./scripts/test.sh           # run the test suite + smoke-run every example
```

Every change must keep `./scripts/test.sh` green, and a debug build must run the
suite cleanly under AddressSanitizer/UBSan before it lands.

## Where things live

| Path | What |
|------|------|
| `src/lume.c` | the whole implementation: lexer → parser → AST → bytecode compiler → VM |
| `tests/` | `.lm` programs that assert behaviour (exit non-zero on failure) |
| `examples/` | runnable showcase programs |
| `docs/` | language guide, stdlib reference, comparison, roadmap |
| `editors/` | TextMate grammar for syntax highlighting |

The source file is organized top-to-bottom in pipeline order with banner
comments (`Values`, `Lexer`, `AST`, `Parser`, `Compiler`, `Virtual machine`,
`Native functions`, `Driver`). Add code to the section it belongs to.

## Adding a built-in function

1. Write `static Value nat_yourfn(Interp *I, int argc, Value *argv)` in the
   *Native functions* section.
2. Register it in `install_stdlib` with `define_native(I, "yourfn", nat_yourfn, arity)`
   (`arity = -1` for variadic).
3. Add a test in `tests/` and document it in `docs/STDLIB.md`.

## Adding a language feature

A feature usually touches three places, in order: the **parser** (produce an AST
node), the **compiler** (`compile_expr`/`compile_stmt` → emit bytecode), and the
**VM** (`run_vm` → a new `OP_*` case). Keep the bytecode the single source of
truth for semantics.

## Style

- C11, 4-space indent, `snake_case` for functions and variables.
- Match the surrounding code's comment density — explain *why*, not *what*.
- No new external dependencies in the core. The whole point is one `clang` build.

## Coding loop discipline

Before claiming something works: run it. Before releasing: see the
[release rules](docs/ROADMAP.md) and keep the README and docs in sync with the
behaviour you changed.

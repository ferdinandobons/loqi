<div align="center">

# Loqi

**The AI-first programming language — the language you speak to machines.**

*Loqi* (from Latin *loqui*, "to speak") is a small, modern language with batteries
included: an LLM call, an HTTP client, JSON and vectors are part of the language,
not packages you hunt down and install. Simple to read, fast on Apple Silicon,
one binary, no toolchain to assemble.

```loqi
# fetch live data, then let a model explain it — no libraries, no SDK
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
print("{repo.full_name}: {repo.stargazers_count} ⭐")

let poem = ai("Scrivi un haiku sul codice pulito")
print(poem)
```

</div>

---

> **Status: v0.2 — early but real, and it does what it says.** The core language,
> a **bytecode VM** (on par with CPython, ~5× faster than the first cut), and the
> **AI-native layer** (`ai`, `json`, `http`, `read`/`write`, `similarity`) all work
> today — implemented in dependency-free C, built with nothing but `clang`. Every
> claim here is backed by code, tests, and an honest [ROADMAP](docs/ROADMAP.md).

## Why Loqi

Three ideas drive every decision:

1. **AI-first, batteries included.** The things you install separately today —
   an HTTP client, a JSON parser, an LLM SDK, a vector-similarity helper — are
   built into the language and its runtime. `ai("...")` calls a model.
   `json.parse`, `json.stringify`, `http.get`, `http.post`, `similarity`,
   `read`, `write`, `env` are always there. No `pip install`, no `npm i`, no SDK.
2. **Fast on Apple Silicon.** The implementation is C, compiled with
   `-O3 -mcpu=apple-m1 -flto`. The execution engine is moving from a reference
   tree-walker to a bytecode VM tuned for arm64.
3. **Simple to read and write.** No semicolons, no ceremony. Curly-brace blocks,
   `let`/`fn`, string interpolation with `{}`. If you've read code before, you
   can read Loqi.

Loqi is **not** built on top of another language. It is its own grammar,
parser, and runtime.

## Quickstart

Requirements: macOS with the Xcode Command Line Tools (`clang`). Nothing else.

```sh
git clone https://github.com/ferdinandobons/loqi && cd loqi
./scripts/build.sh            # produces ./build/loqi
./build/loqi examples/01_hello.lq
./build/loqi                  # starts the REPL
```

A first program (`hello.lq`):

```loqi
fn saluta(nome) {
  return "Ciao, {nome}!"
}

print(saluta("mondo"))
```

```sh
./build/loqi hello.lq
# Ciao, mondo!
```

## A taste

```loqi
# funzioni di ordine superiore + closure
fn mappa(xs, f) {
  let out = []
  for x in xs { push(out, f(x)) }
  return out
}

let quadrati = mappa([1, 2, 3, 4], fn(x) { return x * x })
print(quadrati)            # [1, 4, 9, 16]

# mappe e iterazione
let utente = { nome: "Ada", anni: 36 }
for chiave in utente {
  print("{chiave}: {utente[chiave]}")
}

# interpolazione con espressioni annidate
print("totale: {join(mappa([1,2,3], fn(x){ return str(x) }), ", ")}")
```

## Documentation

- **[Language guide](docs/LANGUAGE.md)** — the full language reference.
- **[Standard library](docs/STDLIB.md)** — every built-in (incl. the AI layer), with examples.
- **[Cheatsheet](docs/CHEATSHEET.md)** — the whole language on one page.
- **[Why Loqi](docs/WHY-LOQI.md)** — the positioning and the bet.
- **[Comparison](docs/COMPARISON.md)** — Loqi vs Python, JS/Node, Go, Rust, Mojo (honest).
- **[Roadmap & engineering log](docs/ROADMAP.md)** — what's done, what's next, and
  honest benchmarks.
- **[Landing page](web/index.html)** — open `web/index.html` in a browser.
- **[Examples](examples/)** — runnable `.lq` programs (`examples/ai/` for AI demos).

## Project layout

```
src/loqi.c        the reference interpreter (lexer, parser, evaluator)
scripts/build.sh  build script (release / debug)
examples/         runnable example programs
tests/            test suite + runner
docs/             language guide, stdlib reference, roadmap
std/              Loqi-side standard library modules (planned)
```

## License

MIT — see [LICENSE](LICENSE).

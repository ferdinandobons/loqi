<div align="center">

# Loqi

**The AI-first programming language — the language you speak to machines.**

*Loqi* (from Latin *loqui*, "to speak") is a small, modern language with batteries
included: an LLM call, an HTTP client, a JSON parser and vector math are part of the
language, not packages you hunt down and install. Simple to read, fast on Apple
Silicon, one binary, no toolchain to assemble.

```loqi
# Fetch live data, then let a model reason about it.
# No pip install, no SDK, no API client — http, json and ai ARE the language.
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
print("{repo.full_name} — {repo.stargazers_count} ⭐")

let summary = ai("In one tweet, what is {repo.name} and why does it matter?")
print(summary)
```

</div>

---

> **🌐 Live site: [ferdinandobons.github.io/loqi](https://ferdinandobons.github.io/loqi/)**

> **Status: v0.2 — early but real, and it does what it says.** The core language, a
> **bytecode VM** (on par with CPython, ~5× faster than the first cut), the
> **AI-native layer** (`ai`, `json`, `http`, `read`/`write`, `similarity`), **modules**,
> plus **`match`** and **`try`/`catch`** all work today — implemented in dependency-free
> C11, built with nothing but `clang`. Every claim here is backed by code, tests, and an
> honest [ROADMAP](docs/ROADMAP.md).

## Why Loqi

Three ideas drive every decision:

1. **AI-first, batteries included.** The things you install separately today — an HTTP
   client, a JSON parser, an LLM SDK, a vector-similarity helper — are part of the
   language and its runtime. `ai("...")` calls a model. `json.parse`, `json.stringify`,
   `http.get`, `http.post`, `similarity`, `read`, `write`, `env` are always there.
   No `pip install`, no `npm i`, no SDK, no glue code.
2. **Fast on Apple Silicon.** A single-pass compiler lowers your code to bytecode that
   runs on a stack VM tuned for arm64, compiled with `-O3 -mcpu=apple-m1 -flto`. One
   binary, instant startup, no warm-up.
3. **Simple to read and write.** No semicolons, no ceremony. Curly-brace blocks,
   `let`/`fn`, string interpolation with `{}`. If you've read code before, you can
   read Loqi.

Loqi is **not** built on top of another language. It is its own grammar, parser,
bytecode compiler, and virtual machine.

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
fn greet(name) {
  return "Hello, {name}!"
}

print(greet("world"))         # Hello, world!
```

## A taste

**Functions, closures, and collections — no imports.**

```loqi
let nums = [1, 2, 3, 4, 5]

let squares = map(nums, fn(x) { return x * x })
print(squares)                                            # [1, 4, 9, 16, 25]
print("sum of evens: {sum(filter(nums, fn(x){ return x % 2 == 0 }))}")  # 6

# maps iterate cleanly; interpolation accepts any expression
let user = { name: "Ada", role: "engineer" }
for key in user {
  print("{key} -> {user[key]}")
}
```

**Pattern matching and error handling that doesn't bite.**

```loqi
fn classify(n) {
  match n {
    0:       { return "zero" }
    1, 2, 3: { return "small" }
    _:       { return "large" }
  }
}

# errors are values you handle, not crashes you fear
let config = { theme: "dark" }
try {
  config = json.parse(read("config.json"))
} catch e {
  print("config.json missing, using defaults ({e})")
}
print(classify(len(keys(config))))
```

**AI-native: structured data straight out of a model — no SDK, no glue.**

```loqi
let text = "Ada Lovelace, born 1815, is regarded as the first programmer."

# `{ json: true }` steers the model, strips markdown fences, and parses for you.
let person = ai("Extract name and birth_year from: {text}", { json: true })
print("{person.name} was born in {person.birth_year}")   # Ada Lovelace was born in 1815
```

Pass options the same way — `ai(prompt, { model, system, temperature, max_tokens })` —
or just `ai("...")` for a plain text answer. See [`examples/ai/extract.lq`](examples/ai/extract.lq).

**Split your program into modules.**

```loqi
# geometry.lq
let PI = 3.14159
fn area(r) { return PI * r * r }
```

```loqi
# main.lq — import paths resolve relative to this file, so it runs from anywhere
import "geometry.lq" as geo

print("circle area: {geo.area(2)}")                      # circle area: 12.56636
print("module's pi: {geo.PI}")                           # module's pi: 3.14159
```

## Documentation

- **[Language guide](docs/LANGUAGE.md)** — the full language reference.
- **[Standard library](docs/STDLIB.md)** — every built-in (incl. the AI layer), with examples.
- **[Cheatsheet](docs/CHEATSHEET.md)** — the whole language on one page.
- **[Why Loqi](docs/WHY-LOQI.md)** — the positioning and the bet.
- **[Comparison](docs/COMPARISON.md)** — Loqi vs Python, JS/Node, Go, Rust, Mojo (honest).
- **[Roadmap & engineering log](docs/ROADMAP.md)** — what's done, what's next, honest benchmarks.
- **[Examples](examples/)** — runnable `.lq` programs (`examples/ai/` for AI demos).

## Project layout

```
src/loqi.c        the language implementation (lexer, parser, bytecode compiler, VM)
scripts/build.sh  build script (release / debug with ASan + UBSan)
scripts/test.sh   test runner
examples/         runnable example programs (examples/ai/ for AI demos)
tests/            test suite (incl. error-handling tests)
docs/             language guide, stdlib reference, roadmap, comparison
web/              landing page + SEO assets (deployed to GitHub Pages)
editors/          editor syntax highlighting (TextMate grammar)
std/              Loqi-side standard library modules (planned)
```

## License

MIT — see [LICENSE](LICENSE).

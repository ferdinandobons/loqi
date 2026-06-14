# Why Loqi

**The AI-first language — simple to read, fast on Apple Silicon.**

This is the positioning document: what Loqi is for, what bet it makes, and —
just as importantly — what it does not yet do. Every claim here is backed by code,
tests, and the [ROADMAP](ROADMAP.md).

---

## The problem: the AI glue tax

Write almost any AI script today and look at what you actually assembled before
your idea ran:

- an **HTTP library**, to talk to an API,
- a **JSON library**, to read and write the bodies,
- an **LLM SDK**, to call the model,
- and a **venv** or **node_modules** to hold it all together, pinned and locked.

None of that is your idea. It is the tax you pay to express your idea. Every new
machine, every CI job, every "can you run this for me" re-pays it: resolve the
deps, build the environment, hope the versions still agree. The interesting part
of an AI program — fetch something, ask a model about it, parse the answer — is a
handful of lines wrapped in a pile of plumbing.

Loqi's premise is that this plumbing is so universal it should not be plumbing
at all.

## The bet

Make the glue first-class. An LLM call, an HTTP client, a JSON codec, and vector
similarity are not packages you install — they are part of a small, fast, readable
language and its runtime. One native binary, nothing to assemble.

```loqi
# fetch live data, then let a model explain it — no libraries, no SDK
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
print("{repo.full_name}: {repo.stargazers_count} stelle")

let poem = ai("Scrivi un haiku sul codice pulito")
print(poem)
```

No `pip install`, no `npm i`, no SDK import, no venv. If `loqi` is on the machine,
this runs.

## The three strengths

### 1. AI-first, batteries included

The things you reach for an external package for are in the language and runtime:

- `ai(prompt)` / `ai(prompt, model)` — a first-class LLM call. Hits the Anthropic
  Messages API, reads `ANTHROPIC_API_KEY`, defaults to `claude-sonnet-4-6`
  (override per call or via `LOQI_AI_MODEL`).
- `json.parse(str)` / `json.stringify(value)` — JSON to and from native values.
- `http.get(url)` / `http.post(url, body, content_type?)` — an HTTP client.
- `similarity(a, b)` — cosine similarity over numeric vectors, for semantic search.
- `env(name)`, `read(path)`, `write(path, content)` — environment and whole-file I/O.

Because these compose with native values, the common AI shapes collapse to a line
or two — for example, "ask a model for structured data and use it directly":

```loqi
let data = json.parse(ai("Estrai nome e anno come JSON da: " + testo))
print(data.nome)
```

### 2. Fast on Apple Silicon

The reference implementation is dependency-free C11, built with nothing but
`clang` (`-O3 -mcpu=apple-m1 -flto`), producing a single native arm64 binary.
The engine is a real pipeline, not a toy: source → lexer → Pratt parser → AST →
single-pass bytecode compiler → stack VM (locals as stack slots, globals in a hash
table, closures via upvalues).

Honest numbers, measured on Apple Silicon (`process_time`):

| Benchmark        | Loqi   | CPython 3.13 | Node / V8     |
| ---------------- | ------ | ------------ | ------------- |
| `fib(30)`        | 0.097s | 0.094s       | 0.017s (JIT)  |
| tight loop, 50M  | 3.0s   | 4.3s         | —             |

Loqi is roughly **on par with CPython** on recursion and **~1.4× faster on tight
loops** — and ~5× faster than Loqi's own first tree-walking interpreter. The
position is simple: *Python-class speed (often faster on loops) from a single
compiled binary, getting faster each iteration.* V8 wins micro-benchmarks because
it JIT-compiles; Loqi does not (yet). Loqi does not claim to beat Go, Rust, C, or
Node.

### 3. Simple to read and write

No semicolons, no ceremony. `let` to declare, `fn` for functions, curly-brace
blocks, `{expr}` string interpolation. If you have read code before, you can read
Loqi.

```loqi
fn fib(n) {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)
}
print("fib(10) = {fib(10)}")
```

Closures and first-class functions are core, not bolted on:

```loqi
fn contatore() {
  let n = 0
  return fn() { n = n + 1; return n }
}
let next = contatore()
print(next(), next(), next())   # 1 2 3
```

## Who it's for

Developers building AI features who are tired of gluing together an HTTP lib, a
JSON lib, an LLM SDK, and a venv or `node_modules` before they can write the part
they care about. People who like Python's readability but want a single fast
binary they can copy to a machine and run.

## What makes it modern

- **Built-in AI/HTTP/JSON/vectors.** The runtime ships with the batteries; there
  is no dependency graph to resolve.
- **Raw strings.** Backtick strings (`` `...` ``) are verbatim — no escapes, no
  interpolation — so JSON and regex go in literally, without escaping wars:

  ```loqi
  let cfg = json.parse(`{"name": "Ada", "tags": ["x", "y"]}`)
  print(cfg.name)
  ```

  Regular `"..."` strings still interpolate recursively with `{}`.
- **Single binary.** Build once with `clang`, ship one file. No interpreter to
  install alongside, no virtual environment to activate.
- **Honest, review-driven development.** Benchmarks are published with the
  competition that beats them. The roadmap states what is missing. See *The loop*.

The core language is deliberately small but complete: types are `nil`, `bool`,
64-bit `int`, 64-bit `float`, `str`, `list`, `map`, `fn`; control flow is
`if`/`else if`/`else`, `while`, `for ... in`, `break`, `continue`, `return`;
equality is structural for lists and maps; and there are 25+ built-ins
(`print`, `len`, `type`, `push`, `pop`, `keys`, `values`, `has`, `range`,
`split`, `join`, `sqrt`, `floor`, `clock`, `assert`, …).

## Non-goals and current limitations

Honesty is part of the brand. Today, Loqi is **v0.2 — early but real**:

- **No JIT yet.** This is why V8 wins micro-benchmarks. Loqi runs bytecode on a
  stack VM; it does not compile hot paths to machine code.
- **No garbage collector yet.** Memory lives in an arena freed at exit — fine for
  scripts, and a GC is on the roadmap.
- **Single-file module model.** No multi-file import system yet.
- **Apple-Silicon-first.** The binary targets arm64 macOS today.
- **`http`/`ai` use `curl` under the hood.** They shell out to the always-present
  `curl` (arguments are shell-quoted; the LLM body goes through a temp file). A
  native HTTP client and an `embed()` built-in are on the roadmap.

These are deliberate trade-offs for an early, fast-moving project, not hidden
costs. The model is freed at exit, the binary is one file, and the language is
shaped before the runtime is fully hardened.

## The loop

Loqi is developed in a tight cycle: **build → measure → adversarial review → fix
→ repeat.** Every change is measured against real interpreters, not vibes. Then a
multi-agent adversarial code review tries to break it. A recent review raised 29
findings; 28 were confirmed and **all were fixed**, including a critical VM
stack-overflow guard. The code is memory-clean under AddressSanitizer and UBSan,
there is a test suite plus dedicated error-tests, and the public
[ROADMAP](ROADMAP.md) carries the benchmarks with it.

The bet is small and verifiable: take the four things every AI script needs, put
them in the language, keep the language fast and readable, and prove it each
iteration.

---

*See the [Language guide](LANGUAGE.md), the [Standard library](STDLIB.md), and the
[Roadmap](ROADMAP.md). Loqi is MIT-licensed.*

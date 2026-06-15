<div align="center">

# Loqi

[![CI](https://github.com/ferdinandobons/loqi/actions/workflows/ci.yml/badge.svg)](https://github.com/ferdinandobons/loqi/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**The AI-first programming language, the language you speak to machines.**

In a classic language, Python, JavaScript, Go, talking to an AI is something you
*bolt on*: install an SDK, build a client, parse the JSON, validate the shape, wire up
retries and parallelism yourself. **In Loqi it *is* the language.** Calling an LLM,
getting back schema-validated data, fanning calls out in parallel, vector search for
RAG, JSON and HTTP, all built in, no packages to hunt down.

*Loqi* (from Latin *loqui*, "to speak") is small, modern and memory-safe: simple to
read, fast on Apple Silicon, one dependency-free binary, no toolchain to assemble.

```loqi
# Fetch live data, then let a model reason about it.
# No pip install, no SDK, no API client, http, json and ai ARE the language.
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
print("{repo.full_name}, {repo.stargazers_count} ⭐")

let summary = ai("In one tweet, what is {repo.name} and why does it matter?")
print(summary)
```

</div>

---

> **🌐 Live site: [ferdinandobons.github.io/loqi](https://ferdinandobons.github.io/loqi/)**

> **Status: v0.2, small but real, and it does what it says.** A **bytecode VM** with a
> precise **garbage collector**; the **AI-native layer**, `ai` (options + structured
> JSON), **`ai_json(prompt, schema)`** (schema-validated output with auto-retry),
> **parallel model calls** (`ai_all`/`run_all`), **vector search / RAG** (`similarity`,
> `topk`, `normalize`), `json`, `http`; a **linear-time, ReDoS-proof `regex`** engine; a
> modern surface, **arrow functions** (`x => x*2`), **`if`/`match` expressions**,
> null-safety (`?.`/`??`), ranges, the pipe `|>`; **modules**, **`try`/`catch`** with
> backtraces; and a broad standard library (collections, math/random, filesystem +
> `path`, `base64`/`hex`, date/time, subprocess). All implemented in dependency-free
> C11, built with nothing but `clang`, **memory-safe and leak-clean under ASan/UBSan**,
> **CI-green on Apple Silicon**. Every claim is backed by code, tests, and an honest
> [ROADMAP](docs/ROADMAP.md).

## Why Loqi

Three ideas drive every decision:

1. **AI-first, batteries included.** The things you install separately today, an HTTP
   client, a JSON parser, an LLM SDK, a vector-similarity helper, are part of the
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

**Functions, closures, and collections, no imports.**

```loqi
let nums = [1, 2, 3, 4, 5]

# arrow functions keep callbacks tiny
let squares = map(nums, x => x * x)
print(squares)                                            # [1, 4, 9, 16, 25]
print("sum of evens: {sum(filter(nums, x => x % 2 == 0))}")  # 6

# `if` and `match` are expressions, no ternary needed
let parity = n => if n % 2 == 0 { "even" } else { "odd" }
print(map(nums, parity))                                  # [odd, even, odd, even, odd]

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

**AI-native: typed, schema-validated data straight out of a model, no SDK, no glue.**

Getting reliable structured data out of an LLM is the #1 chore in AI apps. In Loqi it's
one call: `ai_json` shows the model your schema, parses the reply (markdown fences and
all), **validates it against the schema, and retries once** if it doesn't conform.

```loqi
let text = "Ada Lovelace, born 1815, is regarded as the first programmer."

let schema = {
  type: "object",
  fields: {
    name:       { type: "string" },
    birth_year: { type: "int" },
  },
  required: ["name", "birth_year"],
}

let person = ai_json("Extract the person from: {text}", schema)
print("{person.name} was born in {person.birth_year}")   # Ada Lovelace was born in 1815
```

Prefer a plain answer? `ai("...")`. Need options? `ai(prompt, { model, system,
temperature, max_tokens, json })`. Validate any value yourself with
`json.validate(value, schema)`. See [`examples/ai/extract.lq`](examples/ai/extract.lq).

**Call the model in parallel.** Slow model calls are the bottleneck in AI work, so
fanning them out is built in, `ai_all` runs them concurrently and returns the
answers in order:

```loqi
let reviews = ["Love it!", "Broke on day one.", "It's fine, nothing special."]
let prompts = map(reviews, r => "Sentiment in one word: {r}")

let labels = ai_all(prompts)            # all calls overlap, ~1 call's latency, not N
for l in labels { print(trim(l)) }      # Positive / Negative / Mixed
```

The runtime stays single-threaded (your logic is never racy); only the blocking
I/O overlaps. The same trick works for any commands via `run_all`. See
[`examples/ai/parallel.lq`](examples/ai/parallel.lq).

**Split your program into modules.**

```loqi
# geometry.lq
let PI = 3.14159
fn area(r) { return PI * r * r }
```

```loqi
# main.lq, import paths resolve relative to this file, so it runs from anywhere
import "geometry.lq" as geo

print("circle area: {geo.area(2)}")                      # circle area: 12.56636
print("module's pi: {geo.PI}")                           # module's pi: 3.14159
```

## Loqi vs Python

The same everyday AI task, *extract structured, typed data from text*, in both
languages. It's the job at the heart of most AI apps, and it's where "batteries
included" stops being a slogan.

**Python**, install an SDK, build the client, hand-craft the prompt, strip the
markdown fences the model adds, parse, and validate the shape yourself:

```python
import os, json, anthropic                       # pip install anthropic

client = anthropic.Anthropic(api_key=os.environ["ANTHROPIC_API_KEY"])
text = "Ada Lovelace, born 1815, is regarded as the first programmer."

resp = client.messages.create(
    model="claude-sonnet-4-6",
    max_tokens=1024,
    messages=[{"role": "user", "content":
        f'Extract JSON {{"name": str, "birth_year": int}} from: {text}. Return only JSON.'}],
)
raw = resp.content[0].text.strip()
if raw.startswith("```"):                          # models love to add code fences
    raw = raw.strip("`").split("\n", 1)[-1].rsplit("```", 1)[0]
data = json.loads(raw)                             # may throw on bad JSON
if not isinstance(data.get("birth_year"), int):   # validate the shape yourself
    raise ValueError("birth_year missing or not an int")
print(data["name"], data["birth_year"])
```

**Loqi**, the model call, JSON, fence-stripping, schema validation, and a retry are
*the language*:

```loqi
let text = "Ada Lovelace, born 1815, is regarded as the first programmer."
let schema = {
  type: "object",
  fields: { name: { type: "string" }, birth_year: { type: "int" } },
  required: ["name", "birth_year"],
}
let p = ai_json("Extract the person from: {text}", schema)
print(p.name, p.birth_year)                        # Ada Lovelace 1815
```

No SDK, no client object, no fence-stripping, no manual validation, no retry loop,
and the result is **guaranteed** to match the schema or it raises. Same story for
fetching (`http.get`), parsing (`json.parse`), parallel calls (`ai_all`), and
semantic search (`topk`): in Python each is a dependency and some glue; in Loqi each
is one built-in.

## What makes Loqi different

Other languages bolt AI on through libraries. Loqi builds it into the core, and pairs
it with a small, modern, memory-safe runtime:

- **The AI layer *is* the language.** `ai`, `ai_json` (schema-validated output),
  `ai_all` (parallel calls), `similarity`, `topk`, `normalize`, `json`, `http` are
  built-ins, not packages, clients, or glue. The thing every AI app does on line one
  is already done.
- **Structured output that's actually reliable.** `ai_json(prompt, schema)` validates
  the model's answer against your schema and retries on a miss. The single biggest
  source of flaky AI code, "the model returned almost-JSON", is handled by the
  runtime, not your error handling.
- **Parallel agents without the ceremony.** `ai_all` fans slow model calls out across
  a thread pool and returns answers in order, while your code stays single-threaded
  and race-free. No async colouring, no event loop, no GIL workaround.
- **RAG without a vector database.** `similarity` + `topk` + `normalize` are enough to
  embed, rank, and retrieve in a few lines, semantic search is a language feature.
- **Safer text processing.** The `regex` engine is a linear-time Thompson NFA: it
  **cannot** catastrophically backtrack, so the ReDoS that hangs Python/JS/Java/PCRE
  on a crafted input simply can't happen here.
- **Modern, expression-oriented surface.** Arrow functions (`x => x*2`), `if`/`match`
  as expressions, the pipe `|>`, null-safety (`?.`/`??`), ranges, string interpolation:
  concise to read and write, no semicolons, no ceremony.
- **Memory-safe and honest.** A precise mark-and-sweep GC, **leak-clean and clean
  under AddressSanitizer/UBSan**, every feature hardened by an adversarial multi-agent
  code review. Dependency-free C11 in one file, built with nothing but `clang`.

In short: the things that make AI software tedious and fragile in other languages,
SDK setup, fence-stripping, schema validation, retries, parallel fan-out, vector
search, ReDoS-safe parsing, are first-class here, in a language that stays small,
fast, and safe.

## Documentation

- **[Language guide](docs/LANGUAGE.md)**, the full language reference.
- **[Standard library](docs/STDLIB.md)**, every built-in (incl. the AI layer), with examples.
- **[Cheatsheet](docs/CHEATSHEET.md)**, the whole language on one page.
- **[Why Loqi](docs/WHY-LOQI.md)**, the positioning and the bet.
- **[Comparison](docs/COMPARISON.md)**, Loqi vs Python, JS/Node, Go, Rust, Mojo (honest).
- **[Roadmap & engineering log](docs/ROADMAP.md)**, what's done, what's next, honest benchmarks.
- **[Examples](examples/)**, runnable `.lq` programs (`examples/ai/` for AI demos).

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
```

## License

MIT, see [LICENSE](LICENSE).

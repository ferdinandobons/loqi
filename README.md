<div align="center">

# Loqi

[![CI](https://github.com/ferdinandobons/loqi/actions/workflows/ci.yml/badge.svg)](https://github.com/ferdinandobons/loqi/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**jq for the text only an LLM can parse.**

Pipe in messy text, get back schema-validated structured rows (NDJSON) your next pipe
stage can trust, or a hard failure naming the field that didn't validate.
One static binary: no Python, no venv, no SDK, no runtime. The script *is* the filter.

```sh
cat tickets.txt | loqi extract.lq > tickets.ndjson
```

```loqi
# extract.lq, one record per input line, validated against a schema, emitted as NDJSON.
let schema = {
  type: "object",
  required: ["id", "severity", "summary"],
  fields: {
    id:       { type: "int", min: 1 },
    severity: { type: "string", enum: ["low", "medium", "high", "critical"] },
    summary:  { type: "string", min_length: 3 },
  },
}
let opts = { model: "claude-sonnet-4-6", temperature: 0 }   # same input -> same row

for line in lines(stdin()) {
  if trim(line) == "" { continue }
  let row = ai_json("Extract id, severity and summary from:\n{line}", schema, opts)
  print(json.stringify(row))   # one NDJSON line, validated
}
```

</div>

**Validated-or-fails.** Every row matches the schema (with one automatic retry), or the
run stops with a caret on the field that broke, so the next pipe stage never eats
half-parsed garbage. Same input plus `temperature: 0` gives the same row, so the
output is reproducible. The whole filter is the script above: no client to build, no
fences to strip, no retry loop to hand-roll.

## The one job

`ai()`, `ai_json(prompt, schema)`, `ai_all` (parallel), plus `stdin()` and `lines()`,
turn free-form text into typed, validated rows in one pipe stage. That is the job Loqi
is built for, and the wedge is simple: **typed + validated-or-fails + parallel, in one
binary, where the script itself is the filter.**

| Tool | What it is | Why it falls short here |
| --- | --- | --- |
| **jq** | filter for JSON | needs structured input; Loqi *produces* the structure from prose |
| **grep / awk** | line/field tools | match and slice text, but can't reason about it |
| **Python + SDK** | full language | venv/pip/import tax in a pipe, then you hand-roll the client, JSON parse, schema check, retries |
| **Simon Willison `llm`** | pip CLI, huge ecosystem | a pip install with plugins; no typed schema-validated-or-fails contract |
| **charm `mods`** | static Go binary, great UX | a single static binary too, but returns text, not schema-validated typed rows |
| **Loqi** | static binary + tiny language | one binary, typed, validated-or-fails, parallel; the script *is* the filter |

The runtime needs only `curl` on PATH and `ANTHROPIC_API_KEY` in the environment.
Instant startup, single static binary, macOS and Linux.

## Install

Requirements to build: a C compiler (`clang`; the Xcode Command Line Tools on macOS).
Nothing else.

```sh
git clone https://github.com/ferdinandobons/loqi && cd loqi
./scripts/build.sh            # produces ./build/loqi
sudo cp build/loqi /usr/local/bin/    # optional: put loqi on PATH
```

To run anything that calls a model, set a key (it only ever goes into a 0600 temp
config file, never argv):

```sh
export ANTHROPIC_API_KEY=sk-ant-...
```

## Usage

Run a script as a pipe filter, or inline:

```sh
cat tickets.txt | loqi extract.lq > tickets.ndjson    # script is the filter
loqi -e 'print(ai("one word for the mood of: it works"))'   # inline program
loqi extract.lq                                        # reads stdin, writes stdout
loqi                                                   # REPL
```

**Try it without spending a cent.** `--dry-run` stubs every model call so you can wire
up and test the pipe, and `--limit N` caps how many real calls a run may make (the
same guards exist as `LOQI_AI_DRY_RUN` and `LOQI_AI_MAX_CALLS`):

```sh
cat tickets.txt | loqi --dry-run extract.lq      # no API calls, exercises the pipe
cat tickets.txt | loqi --limit 5 extract.lq      # stop after 5 real model calls
```

Other flags:

```sh
loqi --help        # usage and flags
loqi --version     # version string
```

Exit codes are pipe-friendly: `0` success, `64` usage error, `65` a syntax/compile error, `70` a runtime error (where a row that fails schema
validation lands, with the offending field named), `74` an I/O error. A non-zero
exit means the structured output is not trustworthy, so a failing run never silently
feeds garbage downstream.

## Also in the language

Loqi (from Latin *loqui*, "to speak") is a real, small, memory-safe language, not a
single CLI flag. The extraction job is the point, but the surface underneath it is a
genuine programming language:

- **Modern syntax**: `let`/`const`, `fn`, arrow functions (`x => x * 2`), `if`/`match`
  as expressions, the pipe `|>`, null-safety (`?.`/`??`), string interpolation `{}`,
  modules, `try`/`catch`, implicit line continuation. No semicolons, no ceremony.
- **AI layer**: `ai(prompt, opts)` for a plain answer, `ai_json(prompt, schema, opts)`
  for schema-validated structured output with auto-retry, `ai_all(prompts)` to run
  calls in parallel and get answers back in order, and `ai_json_all(prompts, schema)`
  for parallel **and** validated extraction (fails naming the row + field that broke).
  Pass `{ usage: true }` in opts to get token counts back.
- **Schema constraints**: `type`, `enum`, `required`, `fields`, `items`, `pattern`,
  `min_length`/`max_length`, `min`/`max`. Validate any value yourself with
  `json.validate(value, schema)`.
- **Data and I/O**: `json.parse`/`json.stringify`, `http.get`/`http.post`/`http.serve`,
  plus collections, math, and the usual standard library.
- **RAG helpers**: `similarity`, `topk`, `normalize` for ranking and retrieval.
- **Linear-time regex**: a Thompson-NFA engine that cannot catastrophically backtrack,
  so a crafted input can't trigger ReDoS (no `{n,m}` repetition yet).
- **Best-in-class errors**: syntax *and* runtime errors report `file:line:column`,
  print the offending source line, and underline the exact spot with a `^` caret.

A taste, past the one job:

```loqi
let nums = [1, 2, 3, 4, 5]
let squares = map(nums, x => x * x)         # [1, 4, 9, 16, 25]
let parity = n => if n % 2 == 0 { "even" } else { "odd" }

let labels = ai_all(map(["love it", "broke on day one"], r => "Sentiment in one word: {r}"))
for l in labels { print(trim(l)) }          # all calls overlap, ~1 call's latency
```

The runtime stays single-threaded (your logic is never racy); only the blocking I/O in
`ai_all` overlaps. See [`docs/LANGUAGE.md`](docs/LANGUAGE.md) for the full reference.

## Status

**v0.2, small but real, and it does what it says.** A single-pass bytecode compiler and
stack VM with a precise mark-and-sweep garbage collector, all in dependency-free C11,
built with nothing but `clang`. Memory-safe and leak-clean under AddressSanitizer and
UBSan; CI-green on macOS arm64 and Linux (glibc); roughly CPython-class speed with
instant startup.

Honest about the edges: `ai_all` batches calls, it does not stream tokens. There are no
embeddings, no sandbox, no Windows build, and no package manager. Every claim here is
backed by code, tests, and an honest [ROADMAP](docs/ROADMAP.md).

## Documentation

- **[Language guide](docs/LANGUAGE.md)**, the full language reference.
- **[Standard library](docs/STDLIB.md)**, every built-in (incl. the AI layer), with examples.
- **[Cheatsheet](docs/CHEATSHEET.md)**, the whole language on one page.
- **[Roadmap & engineering log](docs/ROADMAP.md)**, what's done, what's next, honest benchmarks.
- **[Examples](examples/)**, runnable `.lq` programs ([`examples/ai/extract.lq`](examples/ai/extract.lq) is the flagship).

## Project layout

```
src/loqi.c        the language implementation (lexer, parser, bytecode compiler, VM, GC, stdlib)
scripts/build.sh  build script (release / debug with ASan + UBSan)
scripts/test.sh   test runner
examples/         runnable example programs (examples/ai/ for AI demos)
tests/            test suite (incl. error-handling tests)
docs/             language guide, stdlib reference, cheatsheet, roadmap
web/              landing page + SEO assets (deployed to GitHub Pages)
editors/          editor syntax highlighting (TextMate grammar)
```

## License

MIT, see [LICENSE](LICENSE).

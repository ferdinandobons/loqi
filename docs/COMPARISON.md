# Loqi vs Python, JavaScript/Node, Go, Rust, Mojo

An honest comparison. Loqi is **v0.2, early but real**: the core language, a
bytecode VM, and the AI-native layer all work today. The languages below are
mature, battle-tested ecosystems. This page is meant to help you decide *when
Loqi is the right tool* and, just as importantly, when it is not.

The short version: Loqi's bet is **the AI-native surface built into the
language**, an LLM call, an HTTP client, a JSON codec, and vector similarity
that are always there, with no SDK to install and no `venv`/`node_modules` to
assemble. On raw speed it sits in Python's class (often faster on tight loops),
and Go, Rust, and Node will out-run it. All five comparison languages are far
more mature. Where Loqi wins, it wins on **time-to-first-AI-feature and a single
self-contained binary**, not on benchmarks or ecosystem size.

---

## Positioning at a glance

| | **Loqi** | **Python** | **JS / Node** | **Go** | **Rust** | **Mojo** |
|---|---|---|---|---|---|---|
| `ai()` LLM call built in | **✅ yes** | ❌ SDK (`anthropic`) | ❌ SDK (`@anthropic-ai/sdk`) | ❌ SDK / HTTP | ❌ crate / HTTP | ❌ SDK / HTTP |
| JSON built in | **✅ yes** | ✅ stdlib (`json`) | ✅ built in (`JSON`) | ✅ stdlib (`encoding/json`) | ❌ crate (`serde_json`) | ❌ library |
| HTTP client built in | **✅ yes** (via curl) | ⚠️ `urllib` (most use `requests`) | ⚠️ `fetch` (modern runtimes) | ✅ stdlib (`net/http`) | ❌ crate (`reqwest`) | ❌ library |
| Vector similarity built in | **✅ `similarity()`** | ❌ NumPy / SciPy | ❌ library | ❌ library | ❌ crate | ⚠️ tensor-oriented |
| Single native binary | **✅ yes** | ❌ interpreter + files | ❌ runtime + files | **✅ yes** | **✅ yes** | ✅ yes |
| Dependency manager needed | **❌ none** | ✅ pip/venv/poetry | ✅ npm/pnpm/yarn | ✅ go modules | ✅ cargo | ✅ |
| Readability | **high** (Python-like) | high | medium | medium-high | medium-low | medium |
| Speed class | **Python-class** (often faster on loops) | Python-class | fast (JIT) | fast (compiled) | fastest (compiled) | very fast (compiled) |
| Garbage collector | ✅ precise mark-sweep | ✅ | ✅ | ✅ | ❌ (ownership) | ⚠️ (ownership model) |
| JIT | ❌ not yet | ⚠️ (3.13+ experimental) | ✅ (V8) | ❌ (AOT) | ❌ (AOT) | ❌ (AOT) |
| Maturity | **new (v0.2)** | very mature (30+ yrs) | very mature | mature | mature | young (but well-funded) |
| Platforms | Apple Silicon (arm64) only | everywhere | everywhere | everywhere | everywhere | Linux/macOS, growing |
| License | MIT | open | open | open | open | proprietary parts |

**Reading the table honestly:**

- **Loqi's genuine wins** are the leftmost rows: `ai()`, JSON, an HTTP client,
  and `similarity()` are all *in the language*, with **zero dependency manager**.
  That is the whole pitch, no `pip install`, no `npm i`, no SDK, no `venv`.
- **Where the others win**, and they do: Go, Rust, and Node's V8 are **faster**
  than Loqi (Node JITs; Go and Rust compile to optimized machine code). All five
  are **far more mature**, run on **every platform**, and have **enormous
  ecosystems**. Loqi has none of that yet, runs on **Apple Silicon only**, and
  has **no JIT** today (it does have a precise mark-sweep GC).
- Loqi's HTTP is marked "via curl" on purpose: `http`/`ai` shell out to the
  always-present `curl` today (a native client is on the roadmap). It works
  end-to-end and is memory-clean under ASan/UBSan, but it's honest to say it's
  not yet a native networking stack.

---

## Speed, measured (not claimed)

All on Apple Silicon, single thread, `process_time`:

| Workload | Loqi v0.2 | CPython 3.13 | Node (V8 JIT) |
|---|---:|---:|---:|
| `fib(30)` | 0.097 s | 0.094 s | 0.017 s |
| tight loop to 50M | 3.0 s | 4.3 s | n/a |

Loqi is **on par with CPython** on recursion and **~1.4× faster** on tight
numeric loops, and **~5× faster** than its own first tree-walking interpreter.
**V8 is faster** on `fib` because it JIT-compiles hot code; **Loqi does not JIT
yet**. The honest positioning: *Python-class speed (often faster on loops) from a
single compiled binary, getting faster each iteration*, **not** faster than Go,
Rust, C, or Node.

---

## If you're coming from…

### Python
You'll feel at home immediately: `let`/`fn`, no semicolons, clean blocks, string
interpolation, first-class functions, closures, lists and maps. What Loqi
**adds**: `ai()`, `json`, `http`, and `similarity()` are *in the language*, no
`pip install anthropic`, no `requests`, no `venv`, and it ships as one binary
that's often faster than CPython on loops. What you'd **give up**: Python's
colossal ecosystem (NumPy, pandas, Django, the lot), decades of maturity, and the
ability to run anywhere. Loqi is Apple-Silicon-only for now.

### JavaScript / Node
You keep the lightweight feel and first-class functions, and you drop
`node_modules`, `package.json`, and the SDK-per-API ritual entirely, `ai()` and
`http.get()` are built in. What you'd **give up**: V8's JIT (Node is faster on
hot code), the npm universe, async/await and the event loop, and cross-platform
reach. Loqi is synchronous and single-binary, which is simpler but less powerful
for high-concurrency servers.

### Go
You keep the single-binary deployment story and a readable, no-ceremony syntax,
and you gain the AI-native batteries without reaching for an SDK or hand-rolling
HTTP+JSON glue. What you'd **give up**: real compiled-language speed,
goroutines/channels, a huge standard library, and production maturity across
every platform. Go is the better choice for
networked services today; Loqi is the faster path to an AI script or prototype.

### Rust
You trade Rust's correctness-by-construction and top-tier performance for
something dramatically simpler to write. No borrow checker, no `Cargo.toml`, no
`serde` + `reqwest` + an LLM crate to wire together, `json.parse`,
`http.get`, and `ai()` are one call each. What you'd **give up**: Rust's speed,
memory-safety guarantees, fearless concurrency, and a mature crate ecosystem.
Loqi has a GC but no ownership model, so it is not a systems language: it's a
scripting language for AI work.

### Mojo
Both are young languages betting on a specific niche. Mojo's bet is
**Python-superset performance for ML/AI compute** (kernels, tensors,
hardware). Loqi's bet is **AI-native glue**: calling models, parsing their
JSON, and doing semantic search with zero setup. What you'd **give up** choosing
Loqi: Mojo's serious numeric performance and its Python-superset
interoperability. What Loqi gives you instead: a tiny, dependency-free,
MIT-licensed binary where `ai("...")` is a keyword-level convenience, not a
library import. Different problems, Mojo makes the math fast; Loqi makes the
LLM call trivial.

---

## When NOT to use Loqi (yet)

Be honest with yourself. Pick something else if:

- **It's production-critical.** Loqi is v0.2: young, single-platform, and
  small-ecosystem. Use Go, Rust, or a mature runtime for anything that has to
  stay up under heavy production load.
- **You're not on Apple Silicon.** The reference implementation targets arm64
  macOS today. No Linux, Windows, or x86 builds yet. If you need to deploy
  broadly, this is a hard blocker.
- **You need a JIT.** No JIT means V8 will out-run you on hot numeric code (it is
  on the roadmap). Loqi does reclaim memory with a precise mark-sweep GC, so
  allocation-heavy workloads are fine; raw hot-loop throughput is the gap.
- **You depend on a large ecosystem.** No NumPy, no pandas, no web framework, no
  package registry. The single-file module model and the built-in stdlib are
  deliberately small. If your project lives on third-party libraries, Loqi can't
  serve it yet.
- **You need a native, audited networking stack.** `http`/`ai` shell out to
  `curl` today. It works and it's memory-clean, but it's not a native client and
  it assumes `curl` is present.

Where Loqi **does** fit right now: AI scripts and prototypes, glue between a
model and a JSON API, semantic-search experiments, internal tools on a Mac, any
place where the cost of assembling an HTTP lib + a JSON lib + an LLM SDK + a
`venv` outweighs the value of a mature ecosystem.

---

## The "glue tax," side by side

The same tiny task in both languages: **fetch JSON from an API, then ask an LLM
to summarize it.** This is the exact workflow Loqi was built to remove the
friction from.

### Loqi

No installs, no SDK, no virtual environment. `http`, `json`, and `ai` are part
of the language; `ANTHROPIC_API_KEY` is read from the environment automatically.

```loqi
# fetch live data, then let a model summarize it
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
let prompt = "In one sentence, describe this project: {repo.full_name}, {repo.stargazers_count} stars. {repo.description}"
print(ai(prompt))
```

Run it:

```sh
./build/loqi summary.lq
```

### Python (realistic)

First the setup that doesn't appear in the code but is part of the real cost:

```sh
python -m venv .venv
source .venv/bin/activate
pip install requests anthropic        # two third-party deps + their transitive deps
```

Then the program:

```python
import os
import requests
from anthropic import Anthropic

# fetch live data
resp = requests.get("https://api.github.com/repos/python/cpython", timeout=30)
resp.raise_for_status()
repo = resp.json()

# ask an LLM to summarize it
client = Anthropic(api_key=os.environ["ANTHROPIC_API_KEY"])
message = client.messages.create(
    model="claude-sonnet-4-6",
    max_tokens=256,
    messages=[{
        "role": "user",
        "content": (
            f"In one sentence, describe this project: "
            f"{repo['full_name']}, {repo['stargazers_count']} stars. "
            f"{repo.get('description', '')}"
        ),
    }],
)
print(message.content[0].text)
```

**What Loqi removed:** a virtual environment, two `pip` dependencies (`requests`,
`anthropic`) plus their transitive trees, the client-object boilerplate
(`Anthropic(...)`, `messages.create`, `max_tokens`, the `messages` list shape),
and digging the text back out of `message.content[0].text`. In Loqi the HTTP
call, the JSON parse, and the LLM call are three built-in calls and the model's
answer comes back as a plain string.

**What Python keeps that Loqi doesn't have (yet):** explicit timeouts and
`raise_for_status()` error handling, fine-grained control over `max_tokens` and
the message structure, retries/streaming via the SDK, and, of course, the
ability to run anywhere and lean on a vast ecosystem. The Python version is more
verbose because it's more configurable and more mature. Loqi trades that
configurability for a near-zero-setup path to the same result.

---

*Every Loqi claim here is backed by the code, the test suite, and the honest
[ROADMAP](ROADMAP.md). See also the [language guide](LANGUAGE.md) and the
[standard library reference](STDLIB.md).*

# Loqi Standard Library

Everything here is **built in**, always available, no import, no packages. This
is the "batteries included" core, including the **AI-native layer** (`ai`, `json`,
`http`, ...) that you would otherwise assemble from separate libraries.

## Conventions

- `arg: type` describes an expected argument type.
- A trailing `...` means variadic.
- Negative list/string indices count from the end.

## I/O

### `print(...)` → nil
Prints all arguments separated by spaces, followed by a newline.
```loqi
print("x =", 42)        # x = 42
```

### `input(prompt?) → str | nil`
Reads one line from stdin (without the trailing newline). Optional prompt is
printed first. Returns `nil` at end of input.
```loqi
let name = input("What's your name? ")
```

## Conversion & reflection

### `str(v) → str`
Converts any value to its textual form. Used implicitly by string interpolation.

### `int(v) → int`
Converts `float` (truncates), `bool`, or numeric `str` to `int`.

### `float(v) → float`
Converts `int` or numeric `str` to `float`.

### `type(v) → str`
Returns the type name: `"nil" | "bool" | "int" | "float" | "str" | "list" | "map" | "fn"`.

## Collections

### `len(x) → int`
Length of a `str` (bytes), `list`, or `map`.

### `push(list, ...) → list`
Appends one or more values to a list (mutates in place) and returns it.

### `pop(list) → value`
Removes and returns the last element. Errors on an empty list.

### `keys(map) → list` / `values(map) → list`
The map's keys / values, in insertion order.

### `has(coll, x) → bool`
For a `map`: whether the key `x` exists. For a `list`: whether `x` is an element.

### `range(stop)` / `range(start, stop)` / `range(start, stop, step) → list`
A list of ints from `start` (default 0) up to but excluding `stop`, stepping by
`step` (default 1; may be negative).
```loqi
range(3)            # [0, 1, 2]
range(2, 5)         # [2, 3, 4]
range(10, 0, -2)    # [10, 8, 6, 4, 2]
```

## Strings

### `upper(s) → str` / `lower(s) → str`
Case conversion (ASCII).

### `split(s, sep) → list`
Splits `s` on every occurrence of `sep`. An empty `sep` splits into characters.
```loqi
split("a,b,c", ",")     # ["a", "b", "c"]
```

### `join(list, sep) → str`
Joins a list into a string with `sep` between elements (elements are `str`-ified).
```loqi
join([1, 2, 3], "-")    # "1-2-3"
```

## Math

### `abs(x) → number`
Absolute value (preserves int/float).

### `sqrt(x) → float`
Square root.

### `floor(x) → int`
Largest integer ≤ x.

## Time

### `clock() → float`
CPU time in seconds since process start, handy for micro-benchmarks.
```loqi
let t = clock()
# ... work ...
print("elapsed: {clock() - t}s")
```

### `now() → float`
Wall-clock seconds since the Unix epoch (UTC), with sub-second precision, good
for timestamps and for timing real (wall-clock) elapsed time.

### `time`, calendar helpers (UTC)
| Function | Result |
|----------|--------|
| `time.iso(secs?)` | ISO-8601 string `YYYY-MM-DDTHH:MM:SSZ` (default: now) |
| `time.parts(secs?)` | map: `year, month, day, hour, minute, second, weekday (0=Sun), yearday` |
| `time.format(secs, fmt)` | `strftime`-style formatting |
| `time.make(year, month, day [, hour, min, sec])` | build Unix seconds from components (UTC) |
| `time.parse(str [, fmt])` | parse a date string to Unix seconds (UTC) |

`time.parse` accepts ISO-8601 by default (`2026-06-15T09:30:00Z`, `2026-06-15 09:30:00`,
or date-only `2026-06-15` → midnight); pass a `strptime` format for anything else. The
whole string must match, so a partial/garbled date raises rather than parsing silently.
`time.make`/`time.parse` and `time.iso`/`time.format` round-trip.
```loqi
print(time.iso())                          # e.g. 2026-06-15T09:30:00Z
let d = time.parts(now())
print("{d.year}-{d.month}-{d.day}")
print(time.format(now(), `%A, %B %d`))     # e.g. Monday, June 15

let t = time.make(2026, 6, 15, 9, 30, 0)   # -> Unix seconds
print(time.iso(t))                         # 2026-06-15T09:30:00Z
print(time.parse("2026-06-15T09:30:00Z") == t)   # true
print(time.parse("14/11/2023", `%d/%m/%Y`))      # custom format
```

## Regex

A built-in, dependency-free regex engine. It is **linear-time** (a Thompson NFA /
Pike VM): unlike the backtracking engines in Python, JavaScript, Java or PCRE, it
**cannot catastrophically backtrack**, so a pattern like `(a+)+$` against a long
non-matching string returns instantly instead of hanging, there is no "ReDoS".
Matching is leftmost-first with greedy `*` `+` `?` (the semantics you expect from
Perl/Python). Use raw strings (backticks) so backslashes need no escaping.

| Function | Result |
|----------|--------|
| `regex.test(pattern, text)` | `true` if the pattern matches anywhere |
| `regex.find(pattern, text)` | the first matched substring, or `nil` |
| `regex.find_all(pattern, text)` | a list of all non-overlapping matches |
| `regex.replace(pattern, text, repl)` | `text` with every match replaced by `repl` |
| `regex.split(pattern, text)` | `text` split on each match |

Supported syntax: literals, `.` (any byte but newline), `*` `+` `?`, `|`, `(...)`
groups, `[...]` / `[^...]` classes with `a-z` ranges, the anchors `^` and `$`, and
the escapes `\d \D \w \W \s \S \n \t \r` plus any escaped metacharacter (`\.`, `\(`,
…). Matching is byte-oriented (a multi-byte UTF-8 character spans several bytes, and
`.` matches one byte). An invalid pattern raises a catchable error.

`{n,m}` counted repetition is not supported yet; use `*`/`+`/`?` (or repeat the
atom). Replacement is literal, backreferences like `$1` are not supported yet.

```loqi
regex.test(`^(yes|no)$`, "yes")            # true
regex.find(`\w+@\w+`, "ping me at a@b ok") # "a@b"
regex.find_all(`\d+`, "a1 b22 c333")       # ["1", "22", "333"]
regex.replace(`\s+`, "a   b  c", " ")      # "a b c"
regex.split(`,\s*`, "a, b,c,  d")          # ["a", "b", "c", "d"]
```

## Testing

### `assert(cond, message?) → nil`
Aborts with a runtime error if `cond` is falsey. Used by the test suite.
```loqi
assert(2 + 2 == 4, "math is broken")
```

## More collections

| Function | Result |
|----------|--------|
| `sort(list)` | new list sorted ascending (numbers or strings) |
| `reverse(x)` | reversed `list` or `str` |
| `sum(list)` | sum of a numeric list |
| `min(list)` / `max(list)` | extremum of a list |
| `min(a, b, ...)` / `max(a, b, ...)` | extremum of the arguments |
| `slice(coll, start[, end])` | sub-`list`/`str` (negative indices ok) |
| `index_of(coll, x)` | first index of `x` (substring for `str`), or `-1` |
| `contains(coll, x)` | membership in `str` (substring), `list`, or `map` (key) |
| `del(map, key)` | remove a key (mutates), returns the map |
| `zip(a, b)` | list of `[a[i], b[i]]` pairs (stops at the shorter) |
| `enumerate(list)` | list of `[index, value]` pairs |
| `unique(list)` | duplicates removed, first-seen order kept |
| `flatten(list)` | concatenate one level of nested lists |
| `count(list, x)` | how many elements equal `x` |

### Higher-order
| Function | Result |
|----------|--------|
| `map(list, fn)` | apply `fn` to each element → new list |
| `filter(list, fn)` | keep elements where `fn(x)` is truthy |
| `reduce(list, fn, init)` | fold left: `fn(acc, x)` |
| `each(list, fn)` | call `fn(x)` for side effects |
| `find(list, fn)` | first element where `fn(x)` is truthy, else `nil` |
| `any(list, fn)` / `all(list, fn)` | does `fn(x)` hold for any / every element |
| `group_by(list, fn)` | map from `fn(x)` (as a key) → list of its elements |

```loqi
let evens = filter([1, 2, 3, 4], fn(x) { return x % 2 == 0 })   # [2, 4]
let total = reduce([1, 2, 3], fn(a, b) { return a + b }, 0)       # 6
print(sort(map([3, 1, 2], fn(x) { return x * 10 })))             # [10, 20, 30]

let groups = group_by([1, 2, 3, 4, 5], fn(n) { return n % 2 })   # {"1": [1,3,5], "0": [2,4]}
print(zip(["a", "b"], [1, 2]))                                    # [["a", 1], ["b", 2]]
```

## More strings
| Function | Result |
|----------|--------|
| `trim(s)` | strip leading/trailing whitespace |
| `replace(s, old, new)` | replace all occurrences |
| `starts_with(s, p)` / `ends_with(s, p)` | prefix/suffix test |
| `repeat(s, n)` | `s` repeated `n` times |
| `chars(s)` | list of single-character strings |
| `pad_start(s, w[, fill])` / `pad_end(s, w[, fill])` | pad to width `w` with `fill` (default space) |
| `lines(s)` | split into lines (handles CRLF; drops a trailing newline's empty) |
| `words(s)` | split on whitespace runs, dropping empties |

## More math
| Function | Result |
|----------|--------|
| `round(x)` | nearest integer |
| `ceil(x)` | smallest integer ≥ x |
| `pow(a, b)` | `a` to the power `b` (float) |
| `sin(x)` `cos(x)` `tan(x)` | trigonometric functions (radians) |
| `log(x)` / `log(x, base)` | natural log, or log to `base` |
| `exp(x)` | e to the power `x` |
| `now()` | wall-clock seconds (Unix time) |

Constants `PI` and `E` are predefined globals.

## Random
| Function | Result |
|----------|--------|
| `random()` | a float uniformly in `[0, 1)` |
| `randint(a, b)` | a random int in `[a, b]` (inclusive) |
| `seed(n)` | seed the generator (same seed → same sequence) |

```loqi
seed(42)
let roll = randint(1, 6)          # reproducible dice roll
let jitter = random() * 0.1
```

---

# AI-first batteries

The reason Loqi exists. These are **part of the language**, not packages.

## `ai(prompt) → str` / `ai(prompt, model) → str` / `ai(prompt, options) → str | value`
Calls a large language model and returns its answer. Reads `ANTHROPIC_API_KEY`
from the environment; the model defaults to `claude-sonnet-4-6` (override with the
2nd argument or the `LOQI_AI_MODEL` env var).
```loqi
let answer = ai("Explain recursion to a 10-year-old")
print(answer)

let fast = ai("Summarize: {text}", "claude-haiku-4-5")
```
**Multi-turn:** pass a list of messages instead of a string to continue a
conversation:
```loqi
let answer = ai([
  { role: "user", content: "My name is Ada." },
  { role: "assistant", content: "Hi Ada!" },
  { role: "user", content: "What's my name?" },
])
```

The second argument can also be an **options map**:

| key | type | meaning |
|-----|------|---------|
| `model` | str | model name |
| `system` | str | system prompt |
| `temperature` | float | sampling temperature |
| `max_tokens` | int | response cap (default 1024) |
| `json` | bool | return parsed native data instead of text |

With `{ json: true }`, `ai` steers the model toward JSON, tolerates ```` ```json ````
fences, and returns native Loqi values, no manual parsing:
```loqi
let person = ai("Extract name and birth_year from: {text}", { json: true })
print("{person.name} ({person.birth_year})")          # person.birth_year is an int

let summary = ai("Summarize this.", { system: "Be terse.", temperature: 0.2 })
```

**Reliability.** `ai` (and `ai_all`) retries transient failures automatically with
exponential backoff and jitter: HTTP `429` (rate limit), any `5xx`, and transient
network errors (DNS, connect, timeout, dropped connection). A fatal `4xx` (bad
request, auth) is **not** retried, it fails immediately with the API's message.
Tunable via the environment:

| env var | default | meaning |
|---------|---------|---------|
| `LOQI_AI_MAX_RETRIES` | `3` | retries after the first try (0 disables; max 8) |
| `LOQI_AI_RETRY_BASE_MS` | `500` | backoff base in ms; doubles per retry, capped at 16s |
| `LOQI_AI_BASE_URL` | Anthropic Messages API | override the endpoint (a gateway/proxy) |

## `ai_all(prompts) → list` / `ai_all(prompts, options) → list`
Runs many model calls **concurrently** and returns the answers in input order,
the same `options` (incl. `json`) apply to every prompt. This is how you fan out
work to the model in parallel instead of one slow call after another:
```loqi
# classify many items at once, N calls overlap instead of running serially
let labels = ai_all([
  "Sentiment of 'I love it' in one word",
  "Sentiment of 'it broke instantly' in one word"
])
for l in labels { print(trim(l)) }

# structured output works per-prompt too
let people = ai_all(["JSON name,age for: Ada, 36", "JSON name,age for: Bob, 25"], { json: true })
print(people[0].name)
```
Built on `run_all`'s thread pool: the model requests overlap, while the Loqi
runtime stays single-threaded. A failed call raises (use `try` to guard a batch).

## `ai_json(prompt, schema) → value` / `ai_json(prompt, schema, options) → value`
**Structured output**, the reliable way to get typed data out of a model. Loqi
shows the model the `schema`, parses its JSON answer, **validates it against the
schema, and automatically retries once** with the validation errors if it doesn't
conform. Returns the validated native value, or raises if the model still can't
produce conforming output.
```loqi
let schema = {
  type: "object",
  fields: {
    name:    { type: "string" },
    age:     { type: "int" },
    hobbies: { type: "list", items: { type: "string" } },
  },
  required: ["name", "age"],
}
let p = ai_json("Extract the person: 'Ada Lovelace, 36, likes math and poetry'", schema)
print(p.name, p.age, p.hobbies)            # Ada Lovelace 36 [math, poetry]
```
Schema shapes (see `json.validate`): primitive `type` (`string`/`str`, `int`,
`float`, `number`, `bool`, `null`, `any`), `enum`, `{type:"list", items: …}`, and
`{type:"object", fields:{…}, required:[…]}`.

## `json.validate(value, schema) → bool`
Checks any value against a schema (the same one `ai_json` uses), handy for
validating model output, API responses, or config. Listed `fields` that are present
must match; `required` fields must exist; extra fields are allowed. A *malformed*
schema (e.g. `required` that isn't a list, `type` that isn't a string) raises a
clear error rather than silently passing, so a schema typo never hides a bug.
```loqi
let s = { type: "object", fields: { id: { type: "int" } }, required: ["id"] }
print(json.validate({ id: 7, extra: "ok" }, s))   # true
print(json.validate({ id: "x" }, s))              # false
```

## `json.parse(str) → value` / `json.stringify(value) → str`
A real JSON codec, built in. `parse` returns native Loqi values
(`map`/`list`/`str`/`int`/`float`/`bool`/`nil`); `stringify` is the inverse.
```loqi
let m = json.parse(`{"ok": true, "n": [1, 2, 3]}`)   # raw string avoids escaping
print(m.n[0])                                          # 1
print(json.stringify({ a: 1, b: [true, nil] }))       # {"a":1,"b":[true,null]}
```
> Tip: write JSON literals with **raw strings** (backticks) so `{`, `}` and `"`
> need no escaping, see the [language guide](LANGUAGE.md#strings).

## `http.get(url) → str` / `http.post(url, body, content_type?) → str`
An HTTP client, built in. Returns the response body.
```loqi
let zen = http.get("https://api.github.com/zen")
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
print("{repo.full_name}: {repo.stargazers_count} ⭐")

let reply = http.post("https://httpbin.org/post", json.stringify({ hi: "loqi" }))
```

## `http.serve(port, handler)`
A built-in HTTP/1.1 server, so an agent, webhook, or API is a Loqi program end to
end (request → `ai()` → response). Blocks and serves forever (Ctrl-C to stop).

`handler` is `fn(request)` where `request` is a map `{ method, path, headers, body }`
(`headers` keys are lowercased). It returns either:

- a **string** → `200 OK`, `text/plain`, that string as the body; or
- a **map** `{ status?, body?, content_type?, headers? }` for full control
  (`status` defaults to `200`, `content_type` to `text/plain; charset=utf-8`).

The handler runs on the main thread (one request at a time). An **uncaught error in
the handler becomes a `500`** and the server keeps running, so a single bad request
never takes the service down. Requests are bounds-checked (64 KB of headers, 32 MB
body cap).

```loqi
fn handler(req) {
  if req.path == "/health" {
    return { content_type: "application/json", body: json.stringify({ ok: true }) }
  }
  if req.method != "POST" { return { status: 405, body: "POST a prompt\n" } }
  return ai(req.body)              # an LLM endpoint in one line
}
http.serve(8080, handler)
```

```sh
curl -s -X POST --data "Write a haiku about the sea" http://127.0.0.1:8080/
```

## Vectors & semantic search (RAG)

Batteries-included building blocks for retrieval-augmented generation: embed your
documents (and the query) into vectors, then rank by similarity, no vector
database required.

### `similarity(a, b) → float`
Cosine similarity between two numeric vectors (`list` of numbers). `0.0` if either
is the zero vector (never divides by zero).
```loqi
print(similarity([1, 0, 1], [1, 0, 1]))   # 1.0
print(similarity([1, 0], [0, 1]))          # 0.0
```

### `topk(query, vectors, k) → list`
The nearest-neighbour primitive: given a `query` vector and a list of candidate
`vectors`, returns the `k` most similar as `{index, score}` maps, sorted by
descending cosine similarity (`k` is clamped to the number of candidates; ties keep
their original order). `index` points back into `vectors`.
```loqi
let docs = [
  { text: "cats and kittens", vec: [1.0, 0.0, 0.0] },
  { text: "dogs and puppies", vec: [0.0, 1.0, 0.0] },
  { text: "felines purr",     vec: [0.9, 0.1, 0.0] },
]
for h in topk([1.0, 0.05, 0.0], map(docs, d => d.vec), 2) {
  print(docs[h.index].text, h.score)        # cats…, then felines…
}
```

### `normalize(vector) → list`
Returns the unit vector (same direction, length 1); the zero vector maps to itself.
Handy before storing or comparing embeddings.
```loqi
print(normalize([3.0, 4.0]))               # [0.6, 0.8]
```

## `env(name) → str | nil`
Reads an environment variable.
```loqi
let key = env("ANTHROPIC_API_KEY")
```

## Process & OS
| Function | Result |
|----------|--------|
| `run(cmd)` | run `cmd` via the shell; returns `{ out: <stdout>, code: <exit> }` |
| `run_all(cmds)` | run a list of commands **concurrently**; list of `{ out, code }` in input order |
| `args()` | list of CLI arguments passed after the script path |
| `exit(code?)` | exit the process (default code `0`) |
| `env(name)` | environment variable, or `nil` |
| `input(prompt?)` | read a line from stdin |

```loqi
let res = run("git rev-parse --short HEAD")
if res.code == 0 { print("commit: {trim(res.out)}") }
for a in args() { print("arg: {a}") }
```
> `run`/`run_all` pass the command to the shell, quote untrusted input yourself.

### Concurrency with `run_all`
`run_all` is Loqi's concurrency primitive: it runs the commands on worker threads
so their **blocking I/O overlaps**, then returns the results in order. This is how
you fan out slow calls, fetch many URLs, or run many model calls, in parallel:
```loqi
# fetch three pages at once instead of one after another
let pages = run_all([
  "curl -s https://example.com/a",
  "curl -s https://example.com/b",
  "curl -s https://example.com/c",
])
for p in pages { print(len(p.out)) }
```
Up to 32 commands run at a time (larger lists run in waves). The Loqi runtime
itself stays single-threaded, so your program logic is never racy.

## `read(path) → str` / `write(path, content) → nil`
Read and write whole files.
```loqi
write("note.txt", "hi from Loqi\n")
print(read("note.txt"))
```

## Filesystem
| Function | Result |
|----------|--------|
| `ls(dir)` | list of entry names in `dir` (excludes `.` and `..`) |
| `exists(path)` | `true` if the path exists |
| `is_dir(path)` | `true` if the path is a directory |
| `mkdir(path)` | create the directory and any missing parents (`mkdir -p`) |
| `rm(path)` | remove a file or empty directory |

## `path`, path manipulation
| Function | Result |
|----------|--------|
| `path.join(a, b, ...)` | join with `/` (an absolute part resets, empties are skipped) |
| `path.dirname(p)` | everything before the last `/` (`.` if none) |
| `path.basename(p)` | the final component |
| `path.ext(p)` | file extension incl. the dot (`""` if none) |

```loqi
let dir = "data/2026"
mkdir(dir)
write(path.join(dir, "log.txt"), "started\n")
for name in ls(dir) {
  if path.ext(name) == ".txt" { print(read(path.join(dir, name))) }
}
```

## `base64` / `hex`, encoding
| Function | Result |
|----------|--------|
| `base64.encode(s)` / `base64.decode(s)` | standard base64 (RFC 4648, padded) |
| `hex.encode(s)` / `hex.decode(s)` | lowercase hex of the bytes |

```loqi
let token = base64.encode("user:secret")        # "dXNlcjpzZWNyZXQ="
print(base64.decode(token))                      # user:secret
print(hex.encode("Loqi"))                        # 4c6f7169
```


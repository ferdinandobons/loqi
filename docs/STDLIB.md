# Loqi Standard Library

Everything here is **built in** — always available, no import, no packages. This
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
let nome = input("Come ti chiami? ")
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
CPU time in seconds since process start — handy for micro-benchmarks.
```loqi
let t = clock()
# ... work ...
print("durata: {clock() - t}s")
```

## Testing

### `assert(cond, message?) → nil`
Aborts with a runtime error if `cond` is falsey. Used by the test suite.
```loqi
assert(2 + 2 == 4, "la matematica è rotta")
```

---

# AI-first batteries

The reason Loqi exists. These are **part of the language**, not packages.

## `ai(prompt) → str` / `ai(prompt, model) → str`
Calls a large language model and returns its text answer. Reads
`ANTHROPIC_API_KEY` from the environment; the model defaults to
`claude-sonnet-4-6` (override with the 2nd argument or the `LOQI_AI_MODEL` env var).
```loqi
let risposta = ai("Spiega la ricorsione a un bambino di 10 anni")
print(risposta)

let veloce = ai("Riassumi: {testo}", "claude-haiku-4-5")
```
Combine with `json` for structured output:
```loqi
let dati = json.parse(ai("Estrai nome e anno come JSON da: {testo}"))
print(dati.nome)
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
> need no escaping — see the [language guide](LANGUAGE.md#strings).

## `http.get(url) → str` / `http.post(url, body, content_type?) → str`
An HTTP client, built in. Returns the response body.
```loqi
let zen = http.get("https://api.github.com/zen")
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
print("{repo.full_name}: {repo.stargazers_count} ⭐")

let reply = http.post("https://httpbin.org/post", json.stringify({ hi: "loqi" }))
```

## `similarity(a, b) → float`
Cosine similarity between two numeric vectors (`list` of numbers) — the core of
semantic search, no library required.
```loqi
print(similarity([1, 0, 1], [1, 0, 1]))   # 1.0
print(similarity([1, 0], [0, 1]))          # 0.0
```

## `env(name) → str | nil`
Reads an environment variable.
```loqi
let key = env("ANTHROPIC_API_KEY")
```

## `read(path) → str` / `write(path, content) → nil`
Read and write whole files.
```loqi
write("note.txt", "ciao da Loqi\n")
print(read("note.txt"))
```

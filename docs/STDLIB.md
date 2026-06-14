# Lume Standard Library (v0.1)

Everything here is **built in** — always available, no import. This is the
"batteries included" core. The AI-native built-ins (`ai`, `embed`, `http`,
`json`, ...) are tracked in the [roadmap](ROADMAP.md) and land in the next
iterations.

## Conventions

- `arg: type` describes an expected argument type.
- A trailing `...` means variadic.
- Negative list/string indices count from the end.

## I/O

### `print(...)` → nil
Prints all arguments separated by spaces, followed by a newline.
```lume
print("x =", 42)        # x = 42
```

### `input(prompt?) → str | nil`
Reads one line from stdin (without the trailing newline). Optional prompt is
printed first. Returns `nil` at end of input.
```lume
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
```lume
range(3)            # [0, 1, 2]
range(2, 5)         # [2, 3, 4]
range(10, 0, -2)    # [10, 8, 6, 4, 2]
```

## Strings

### `upper(s) → str` / `lower(s) → str`
Case conversion (ASCII).

### `split(s, sep) → list`
Splits `s` on every occurrence of `sep`. An empty `sep` splits into characters.
```lume
split("a,b,c", ",")     # ["a", "b", "c"]
```

### `join(list, sep) → str`
Joins a list into a string with `sep` between elements (elements are `str`-ified).
```lume
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
```lume
let t = clock()
# ... work ...
print("durata: {clock() - t}s")
```

## Testing

### `assert(cond, message?) → nil`
Aborts with a runtime error if `cond` is falsey. Used by the test suite.
```lume
assert(2 + 2 == 4, "la matematica è rotta")
```

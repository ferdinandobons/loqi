# Loqi Cheatsheet

The language behind "jq for the text only an LLM can parse": small, readable, one page,
copy-pasteable. File extension `.lq`. Run with `loqi file.lq` (or `cat data | loqi
script.lq`). Comments start with `#`. No semicolons (a newline ends a statement; `;`
also separates). Blocks use `{ }`.

## Variables

```loqi
let name = "world"      # declare with let
name = "hi"            # reassign with = (assigning an undeclared name is an error)
const PI = 3.14159     # immutable: reassigning it is a compile error
let a = 1; let b = 2   # ; separates statements on one line
```

## Types

```loqi
nil                    # nil
true                   # bool
42                     # int (64-bit)
3.14                   # float (64-bit)
"text"                 # str
[1, 2, 3]              # list
{"k": "v"}             # map
fn() { return 1 }      # fn (first-class)
```

```loqi
print(type(42))        # "int"
```

## Operators

```loqi
1 + 2 - 3 * 4          # arithmetic
7 / 2                  # 3.5   -> / is float division
7 // 2                 # 3     -> // is floor division
7 % 2                  # 1     modulo
a == b   a != b        # structural equality (works on lists & maps)
a < b  a <= b  a > b  a >= b
true and false   true or false   !true   # logical
```

## Strings

```loqi
let n = 3
print("I have {n} apples")     # interpolation: {expr} (recursive)
print("total: {n * 2 + 1}")    # any expression inside {}
let raw = `{"name": "Ada"}`    # raw string: backticks, verbatim, no escapes/interp
```

## Lists

```loqi
let xs = [1, 2, 3]
push(xs, 4)            # append
let last = pop(xs)     # remove & return last
print(len(xs))         # length
print(xs[0])           # index
for x in xs { print(x) }
```

## Maps

```loqi
let m = {"name": "Ada", "year": 1815}
print(m["name"])       # index by key
print(m.name)          # dot access
print(has(m, "year"))  # true
print(keys(m))         # list of keys
print(values(m))       # list of values
```

## Control flow

```loqi
if n < 0 {
  print("neg")
} else if n == 0 {
  print("zero")
} else {
  print("pos")
}

while n > 0 { n = n - 1 }

for i in range(0, 5) { if i == 2 { continue } print(i) }

for ch in "ab" { if ch == "b" { break } print(ch) }
```

## Functions, closures, recursion

```loqi
fn fib(n) {
  if n < 2 { return n }
  return fib(n - 1) + fib(n - 2)   # recursion
}
print("fib(10) = {fib(10)}")

let square = fn(x) { return x * x }   # anonymous fn, first-class

fn counter() {
  let n = 0
  return fn() { n = n + 1; return n }  # closure over n
}
let next = counter()
print(next(), next(), next())          # 1 2 3
```

## AI-first builtins (no installs, no SDK)

```loqi
let poem = ai("Write a haiku about clean code")             # LLM call; reads ANTHROPIC_API_KEY
let txt  = ai("Summarize in one line", "claude-sonnet-4-6") # override model per-call

let cfg = json.parse(`{"name": "Ada", "tags": ["x","y"]}`)  # JSON str -> value
let s   = json.stringify({"ok": true})                      # value -> JSON str

let body = http.get("https://api.github.com/repos/python/cpython")   # HTTP GET -> str
let resp = http.post("https://api.example.com", body, "application/json")  # HTTP POST -> str

let score = similarity([1.0, 0.0], [0.9, 0.1])   # cosine similarity over vectors -> float

let key = env("ANTHROPIC_API_KEY")    # read env var
let content = read("note.txt")        # read whole file -> str
write("out.txt", "hi")                # write whole file
```

### Combined: extract structured data from text

```loqi
let data = json.parse(ai("Extract name and year as JSON from: " + text))
print(data.name)
```

### Combined: fetch + parse JSON from the web

```loqi
let repo = json.parse(http.get("https://api.github.com/repos/python/cpython"))
print("{repo.full_name}: {repo.stargazers_count} stars")
```

## More builtins (25+)

`print` `len` `type` `int` `float` `push` `pop` `keys` `values` `has` `range`
`upper` `lower` `split` `join` `abs` `sqrt` `floor` `clock` `input` `assert`

```loqi
print(upper("hi"))                   # "HI"
print(join(split("a,b,c", ","), "-")) # "a-b-c"
print(sqrt(abs(-9)))                 # 3.0
assert(1 + 1 == 2)                   # aborts if false
```

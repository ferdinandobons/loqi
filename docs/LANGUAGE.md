# The Loqi Language Guide (v0.1)

This is the complete reference for the language as it exists today. Anything
marked *planned* lives in the [roadmap](ROADMAP.md), not here — this guide only
documents what runs.

## Contents

1. [Lexical structure](#lexical-structure)
2. [Values and types](#values-and-types)
3. [Variables](#variables)
4. [Operators](#operators)
5. [Strings and interpolation](#strings-and-interpolation)
6. [Collections](#collections)
7. [Control flow](#control-flow)
8. [Functions and closures](#functions-and-closures)
9. [Modules](#modules)
10. [Grammar](#grammar)

---

## Lexical structure

- **Comments** start with `#` and run to end of line.
- **Statements** are terminated by a newline. There are no semicolons.
- A long expression can be continued onto the next line with a trailing `\`.
- Whitespace is otherwise insignificant.

```loqi
# this is a comment
let x = 1 + \
        2          # line continuation
```

## Values and types

Loqi has a small, predictable set of types:

| Type    | Examples                        | Notes                                  |
|---------|---------------------------------|----------------------------------------|
| `nil`   | `nil`                           | absence of a value                     |
| `bool`  | `true`, `false`                 |                                        |
| `int`   | `42`, `-7`, `1_000_000`         | 64-bit signed; `_` allowed as separator|
| `float` | `3.14`, `1e9`, `2.0`            | 64-bit IEEE-754                         |
| `str`   | `"hi"`, `"line\n"`              | UTF-8 bytes; interpolation with `{}`   |
| `list`  | `[1, 2, 3]`                     | growable, heterogeneous                 |
| `map`   | `{ a: 1, b: 2 }`                | string keys, insertion-ordered          |
| `fn`    | `fn(x) { return x + 1 }`        | first-class, closes over its scope      |

`type(v)` returns the type name as a string.

## Variables

Declare with `let`. Assign to an existing variable with `=`. Assigning to an
undeclared name is an error (this catches typos).

```loqi
let n = 10
n = n + 1          # ok
m = 5              # error: 'm' not declared (use 'let')
```

Variables are block-scoped. An inner scope can shadow an outer one.

## Operators

| Category    | Operators                              | Notes                          |
|-------------|----------------------------------------|--------------------------------|
| Arithmetic  | `+  -  *  /  //  %`                     | `/` is float division; `//` floors |
| Comparison  | `==  !=  <  <=  >  >=`                  | numeric and value equality     |
| Logical     | `and  or  not`                         | short-circuit; return operands |
| Unary       | `-x`, `not x`                          |                                |

Notes:

- `+` also concatenates strings (`"a" + "b"`) and lists (`[1] + [2]`).
- `and`/`or` short-circuit and return the deciding operand, so
  `let name = input or "anon"` works as a default.
- Integer arithmetic stays `int`; mixing with a `float` produces a `float`.
- `5 / 2` is `2.5`; `5 // 2` is `2`; `-7 // 2` is `-4` (floor).

## Strings and interpolation

Strings are double-quoted. Any `{expr}` inside a string is evaluated and spliced
in (the value is converted with `str`). Use `\{` and `\}` for literal braces.

```loqi
let a = 6
let b = 7
print("{a} * {b} = {a * b}")          # 6 * 7 = 42
print("set: \{ {a}, {b} \}")          # set: { 6, 7 }
```

Interpolation is fully recursive — expressions may contain their own strings and
nested interpolation:

```loqi
print("names: {join(["Ada", "Lin"], ", ")}")
```

Escapes: `\n  \t  \r  \\  \"  \0  \{  \}`.

### Raw strings

A string in **backticks** is verbatim: no escapes, no interpolation. Ideal for
JSON, regexes, Windows paths, or anything full of `{`, `}`, `"` and `\`. Write a
literal backtick as `` `` `` (two backticks). Raw strings may span multiple lines.

```loqi
let payload = `{"name": "Ada", "tags": ["x", "y"]}`   # no escaping needed
let pattern = `\d+\.\d+`
let doc = `line one
line two`
```

This is why JSON is pleasant in Loqi: `json.parse(`{"k": 1}`)` just works.

### Indexing and iteration

Strings are indexable and iterable by byte/char:

```loqi
let s = "loqi"
print(s[0])           # l
for c in s { print(c) }
```

## Collections

### Lists

```loqi
let xs = [1, 2, 3]
push(xs, 4)           # [1, 2, 3, 4]
let last = pop(xs)    # 4
print(xs[0])          # 1
print(xs[-1])         # 3   (negative indices count from the end)
xs[0] = 10
print(len(xs))        # 3
print(has(xs, 2))     # true
```

### Maps

```loqi
let m = { name: "Ada", age: 36 }
print(m.name)         # dot access for identifier keys
print(m["name"])      # bracket access for any key
m.city = "London"     # add / update
m["country"] = "UK"
print(keys(m))        # ["name", "age", "city", "country"]
print(values(m))
print(has(m, "name")) # true
```

Iterating a map yields its keys.

## Control flow

No parentheses around conditions; blocks are always braced.

```loqi
if x > 0 {
  print("positive")
} else if x == 0 {
  print("zero")
} else {
  print("negative")
}

while x > 0 {
  x = x - 1
}

for i in range(0, 10) {
  if i == 3 { continue }
  if i == 7 { break }
  print(i)
}
```

`for ... in` iterates over lists, maps (keys), and strings. `range(stop)`,
`range(start, stop)` and `range(start, stop, step)` produce lists of ints.

### Pattern matching

`match` compares a value against patterns and runs the first arm that matches.
Patterns are values; `_` is the default. Comma-separate patterns to match any of
them. (Each arm is a block.)

```loqi
fn category(grade) {
  match grade {
    "A", "B": { return "excellent" }
    "C":      { return "sufficient" }
    _:        { return "needs review" }
  }
}
```

### Error handling: `try` / `catch`

A program shouldn't die at the first failed network call or bad JSON. Wrap risky
code in `try`; if anything raises (a runtime error *or* a built-in like
`json.parse`, `http.get`, `ai`), control jumps to `catch`, with the error message
bound to the optional variable.

```loqi
try {
  let data = json.parse(http.get(url))
  print(data.title)
} catch err {
  print("request failed: {err}")
}

# the catch variable is optional
try { risky() } catch { print("handled") }
```

`return` works from inside a `try`. (Avoid `break`/`continue` that jump out of a
`try` block before it finishes — that edge case isn't supported yet.)

An **uncaught** error prints the message and a backtrace of the call stack, so
you see exactly where it came from:

```
runtime error [app.lq:4]: index 3 out of bounds (len 0)
  at deepest (app.lq:4)
  at middle (app.lq:6)
  at outer (app.lq:7)
  at <script> (app.lq:8)
```

## Functions and closures

Declare with `fn name(params) { ... }`. Functions are first-class values and can
be anonymous (`fn(x) { ... }`). They close over the scope in which they are
defined.

```loqi
fn apply(f, x) { return f(x) }
print(apply(fn(n) { return n * 2 }, 21))     # 42

fn counter() {
  let n = 0
  return fn() { n = n + 1; return n }        # captures n
}
let c = counter()
print(c(), c(), c())                          # 1 2 3
```

A function with no `return` returns `nil`. Recursion is supported directly.

## Modules

Loqi has two ways to bring in another file. **Import paths resolve relative to the
file doing the import**, so a module works no matter which directory you run it from.

**Namespaced import** — the recommended form. The module runs in its own global
scope and is exposed under a name; its functions keep resolving *their own* globals,
so a module's internal constants never leak into — or clash with — yours:

```loqi
# geometry.lq
let PI = 3.14159
fn area(r) { return PI * r * r }
```

```loqi
# main.lq
import "geometry.lq" as geo

print(geo.area(2))      # 12.56636 — uses the module's own PI
print(geo.PI)           # 3.14159  — constants are accessible too
```

**Flat import** — runs the file in the *current* global scope, binding its
top-level names directly (handy for a shared prelude):

```loqi
import "geometry.lq"
print(area(2))          # area and PI are now in scope here
```

Two things to know about the current model (a loaded-module cache giving
single-evaluation semantics is on the roadmap):

- **Each import executes the file.** Importing the same module twice runs its
  top-level code twice, so keep import-time side effects light.
- **Cycles are detected, not fatal.** A circular import (`a` imports `b` imports
  `a`) raises a catchable `cyclic import of '...'` error instead of crashing.

## Grammar

An informal EBNF of v0.1:

```ebnf
program     = { statement } ;
statement   = letDecl | fnDecl | ifStmt | whileStmt | forStmt
            | returnStmt | "break" | "continue" | importStmt | block | exprStmt ;
letDecl     = "let" IDENT [ "=" expression ] NEWLINE ;
fnDecl      = "fn" IDENT "(" [ params ] ")" block ;
params      = IDENT { "," IDENT } ;
ifStmt      = "if" expression block [ "else" ( ifStmt | block ) ] ;
whileStmt   = "while" expression block ;
forStmt     = "for" IDENT "in" expression block ;
returnStmt  = "return" [ expression ] NEWLINE ;
importStmt  = "import" STRING NEWLINE ;
block       = "{" { statement } "}" ;
exprStmt    = expression NEWLINE ;

expression  = assignment ;
assignment  = ( IDENT | index | member ) "=" assignment | logicOr ;
logicOr     = logicAnd { "or" logicAnd } ;
logicAnd    = equality { "and" equality } ;
equality    = comparison { ( "==" | "!=" ) comparison } ;
comparison  = term { ( "<" | "<=" | ">" | ">=" ) term } ;
term        = factor { ( "+" | "-" ) factor } ;
factor      = unary { ( "*" | "/" | "//" | "%" ) unary } ;
unary       = ( "-" | "not" ) unary | postfix ;
postfix     = primary { call | index | member } ;
call        = "(" [ args ] ")" ;
index       = "[" expression "]" ;
member      = "." IDENT ;
primary     = INT | FLOAT | STRING | RAWSTRING | "true" | "false" | "nil"
            | IDENT | listLit | mapLit | fnExpr | "(" expression ")" ;
listLit     = "[" [ expression { "," expression } ] "]" ;
mapLit      = "{" [ entry { "," entry } ] "}" ;
entry       = ( IDENT | expression ) ":" expression ;
fnExpr      = "fn" "(" [ params ] ")" block ;
```

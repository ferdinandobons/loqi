# The Loqi Language Guide (v0.2)

This is the complete reference for the language as it exists today. Anything
marked *planned* lives in the [roadmap](ROADMAP.md), not here, this guide only
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

Use `const` for an immutable binding: it must have an initializer, and assigning to
it is a compile-time error (even from inside a closure that captured it). Shadowing
it with a new `let`/`const` in an inner scope is fine, that is a different binding.

```loqi
const PI = 3.14159
PI = 3             # error: cannot assign to constant 'PI'
```

Variables are block-scoped. An inner scope can shadow an outer one.

## Operators

| Category    | Operators                              | Notes                          |
|-------------|----------------------------------------|--------------------------------|
| Arithmetic  | `+  -  *  /  //  %`                     | `/` is float division; `//` floors |
| Comparison  | `==  !=  <  <=  >  >=`                  | numeric and value equality     |
| Logical     | `and  or  not`                         | short-circuit; return operands |
| Null-safety | `?.`  `??`                             | optional chaining; nil-coalescing |
| Range       | `a..b`                                 | inclusive int range → list     |
| Pipeline    | `x \|> f(a)`                            | `f(x, a)`, left-threads `x`    |
| Unary       | `-x`, `not x`                          |                                |

Notes:

- `+` also concatenates strings (`"a" + "b"`) and lists (`[1] + [2]`).
- `and`/`or` short-circuit and return the deciding operand, so
  `let name = input or "anon"` works as a default.
- `a..b` is an **inclusive** integer range that builds a list, `1..3` is
  `[1, 2, 3]`; `5..1` is `[]`. Great in `for`: `for i in 1..n { ... }`. It binds
  looser than arithmetic (`0..n-1` is `0..(n-1)`).
- `x |> f(a)` is the **pipeline** operator: it threads `x` in as the first
  argument, so `x |> f(a)` is `f(x, a)` and `x |> f` is `f(x)`. Chains read
  top-to-bottom and may break before each `|>`:

  ```loqi
  let result = [1, 2, 3, 4, 5]
    |> filter(x => x % 2 == 1)
    |> map(x => x * x)
    |> sum                                  # 1 + 9 + 25 = 35
  ```

### Null-safety: `?.` and `??`

Loqi targets the "billion-dollar mistake" with two operators:

- `a?.b`, **optional chaining**: evaluates to `nil` if `a` is `nil`, otherwise
  `a.b`. Chains short-circuit: `user?.address?.city` is `nil` if any link is nil.
- `x ?? y`, **null-coalescing**: `x` if it is not `nil`, otherwise `y`. Unlike
  `or`, it only replaces `nil`, `false ?? 1` is `false`, `0 ?? 1` is `0`.

```loqi
fn display_name(user) {
  return user?.name ?? "guest"      # nil-safe: no crash if user or name is missing
}
display_name(nil)              # "guest"
display_name({ name: "Ada" })  # "Ada"
```

`?.` guards the member access itself (not a following call or index): `obj?.field`
is nil-safe, but in `obj?.method()` the `?.` only protects reading `method`, pair
it with `??` (`obj?.method ?? default`) when the base may be nil. `?.` is not
allowed on the left of an assignment.
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

Interpolation is fully recursive, expressions may contain their own strings and
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

### `if` and `match` as expressions

`if` and `match` used in expression position evaluate to the value of the taken
branch / matched arm, no separate ternary operator needed. A branch's value is the
last expression in its block; an `if` with no `else` (false condition) or a `match`
with no matching arm evaluates to `nil`.

```loqi
let label = if score >= 90 { "A" } else if score >= 80 { "B" } else { "C" }
let kind  = match n { 0: { "zero" } 1, 2, 3: { "small" } _: { "big" } }
print(map(xs, x => if x % 2 == 0 { "even" } else { "odd" }))
```

They compose anywhere a value is expected, call arguments, list elements, arrow
bodies, nested in each other. A `match` evaluates its subject exactly once. Two small
rules (the VM addresses locals from the frame base): an expression `if` wants `}
else` on the same line, and an expression `if`/`match` branch can't declare local
variables (`let`), use the statement form for that.

### Pattern matching

`match` compares a value against patterns and runs the first arm that matches.
Patterns are values; `_` is the default. Comma-separate patterns to match any of
them. Each arm is a block, and the subject is evaluated exactly once.

```loqi
fn category(grade) {
  match grade {
    "A", "B": { return "excellent" }
    "C":      { return "sufficient" }
    _:        { return "needs review" }
  }
}

# match is also an expression (see above), the matched arm's value is the result:
let category = match grade { "A", "B": { "excellent" } _: { "other" } }
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
`try` block before it finishes, that edge case isn't supported yet.)

An **uncaught** error prints the message, the offending source line, and a
backtrace of the call stack, so you see exactly where it came from:

```
runtime error [app.lq:4]: index 3 out of bounds (len 0)
  4 |   return items[3]
  at deepest (app.lq:4)
  at middle (app.lq:6)
  at outer (app.lq:7)
  at <script> (app.lq:8)
```

A **syntax** error pinpoints the exact column: it reports `file:line:column`, prints
the offending line, and underlines the spot with a `^` caret (Rust/Elm style):

```
syntax error [app.lq:1:14] at ')': expected an expression
  1 | let x = (1 + )
                   ^
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

### Arrow functions

For the common case of a one-expression function, especially callbacks, use the
arrow shorthand. `params => expr` is exactly `fn(params) { return expr }`:

```loqi
let dbl   = x => x * 2          # one parameter, no parentheses
let add   = (a, b) => a + b     # parenthesize two or more
let answer = () => 42           # and the empty parameter list

map([1, 2, 3], x => x * x)      # [1, 4, 9]
reduce(nums, (acc, x) => acc + x, 0)
```

The body is a single expression. Arrows are right-associative, so they curry
naturally, `a => b => a + b` is a function returning a function, and they read
especially well with the pipe operator:

```loqi
[1, 2, 3, 4, 5, 6]
  |> filter(x => x % 2 == 0)
  |> map(x => x * 10)
  |> sum                        # 120
```

For a multi-statement body, use the full `fn(params) { ... }` form.

## Modules

Loqi has two ways to bring in another file. **Import paths resolve relative to the
file doing the import**, so a module works no matter which directory you run it from.

**Namespaced import**, the recommended form. The module runs in its own global
scope and is exposed under a name; its functions keep resolving *their own* globals,
so a module's internal constants never leak into, or clash with, yours. A module
exports exactly the top-level names it defines with `let`/`fn`, including ones that
shadow a built-in (a module may define and export its own `PI`):

```loqi
# geometry.lq
let PI = 3.14159
fn area(r) { return PI * r * r }
```

```loqi
# main.lq
import "geometry.lq" as geo

print(geo.area(2))      # 12.56636, uses the module's own PI
print(geo.PI)           # 3.14159, constants are accessible too
```

**Flat import**, runs the file in the *current* global scope, binding its
top-level names directly (handy for a shared prelude):

```loqi
import "geometry.lq"
print(area(2))          # area and PI are now in scope here
```

Two things to know about the model:

- **A namespaced module evaluates exactly once.** `import "x.lq" as a` is cached by
  the file's canonical path: a second `import "x.lq" as b`, including a *diamond*
  where two modules both import `x.lq`, returns the same already-evaluated module
  instead of re-running its top-level code, so `a` and `b` share one instance of its
  state. (Flat `import "x.lq"` binds names into the *current* scope, so it re-runs
  per importing scope by design.)
- **Cycles are detected, not fatal.** A circular import (`a` imports `b` imports
  `a`) raises a catchable `cyclic import of '...'` error instead of crashing.

## Grammar

An informal EBNF of v0.2:

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

expression  = arrowFn | assignment ;
arrowFn     = ( IDENT | "(" [ params ] ")" ) "=>" expression ;   (* fn(params){ return expr } *)
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

(See *Arrow functions* above: `params => expr` and `(params) => expr` desugar to
`fn(params) { return expr }`.)

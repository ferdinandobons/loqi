# The Lume Language Guide (v0.1)

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

```lume
# this is a comment
let x = 1 + \
        2          # line continuation
```

## Values and types

Lume has a small, predictable set of types:

| Type    | Examples                        | Notes                                  |
|---------|---------------------------------|----------------------------------------|
| `nil`   | `nil`                           | absence of a value                     |
| `bool`  | `true`, `false`                 |                                        |
| `int`   | `42`, `-7`, `1_000_000`         | 64-bit signed; `_` allowed as separator|
| `float` | `3.14`, `1e9`, `2.0`            | 64-bit IEEE-754                         |
| `str`   | `"ciao"`, `"riga\n"`            | UTF-8 bytes; interpolation with `{}`   |
| `list`  | `[1, 2, 3]`                     | growable, heterogeneous                 |
| `map`   | `{ a: 1, b: 2 }`                | string keys, insertion-ordered          |
| `fn`    | `fn(x) { return x + 1 }`        | first-class, closes over its scope      |

`type(v)` returns the type name as a string.

## Variables

Declare with `let`. Assign to an existing variable with `=`. Assigning to an
undeclared name is an error (this catches typos).

```lume
let n = 10
n = n + 1          # ok
m = 5              # errore: 'm' non dichiarata (usa 'let')
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

```lume
let a = 6
let b = 7
print("{a} * {b} = {a * b}")          # 6 * 7 = 42
print("set: \{ {a}, {b} \}")          # set: { 6, 7 }
```

Interpolation is fully recursive — expressions may contain their own strings and
nested interpolation:

```lume
print("nomi: {join(["Ada", "Lin"], ", ")}")
```

Escapes: `\n  \t  \r  \\  \"  \0  \{  \}`.

Strings are indexable and iterable by byte/char:

```lume
let s = "lume"
print(s[0])           # l
for c in s { print(c) }
```

## Collections

### Lists

```lume
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

```lume
let m = { nome: "Ada", anni: 36 }
print(m.nome)         # dot access for identifier keys
print(m["nome"])      # bracket access for any key
m.citta = "Londra"    # add / update
m["paese"] = "UK"
print(keys(m))        # ["nome", "anni", "citta", "paese"]
print(values(m))
print(has(m, "nome")) # true
```

Iterating a map yields its keys.

## Control flow

No parentheses around conditions; blocks are always braced.

```lume
if x > 0 {
  print("positivo")
} else if x == 0 {
  print("zero")
} else {
  print("negativo")
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

## Functions and closures

Declare with `fn name(params) { ... }`. Functions are first-class values and can
be anonymous (`fn(x) { ... }`). They close over the scope in which they are
defined.

```lume
fn applica(f, x) { return f(x) }
print(applica(fn(n) { return n * 2 }, 21))   # 42

fn contatore() {
  let n = 0
  return fn() { n = n + 1; return n }        # captures n
}
let c = contatore()
print(c(), c(), c())                          # 1 2 3
```

A function with no `return` returns `nil`. Recursion is supported directly.

## Modules

`import "path.lm"` executes another file in the current global scope (a simple
v0.1 module model; namespaced modules are on the roadmap).

```lume
import "std/math.lm"
```

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
primary     = INT | FLOAT | STRING | "true" | "false" | "nil"
            | IDENT | listLit | mapLit | fnExpr | "(" expression ")" ;
listLit     = "[" [ expression { "," expression } ] "]" ;
mapLit      = "{" [ entry { "," entry } ] "}" ;
entry       = ( IDENT | expression ) ":" expression ;
fnExpr      = "fn" "(" [ params ] ")" block ;
```

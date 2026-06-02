# c-hcl2 grammar (milestone 1 — expression engine)

This document is the **specification of what c-hcl2 actually parses today**, not
of the full HCL2 language. It is derived directly from the hand-written lexer
(`lexer.c`) and Pratt parser (`parser.c`); when they change, change this too.

For the rationale behind hand-writing the lexer/parser instead of using
flex/bison, see [README.md](README.md) and the design notes. In short: the HCL
reference implementation itself uses a generated *scanner* (ragel) and a
hand-written *parser*, HCL2 string templates are context-sensitive (awkward for
flex), and 100 %-instrumentable code + a zero-build-dependency library are
explicit goals here.

Features still missing (heredocs, `for`-expressions, splat `[*]`, `%{ }`
template directives, the JSON profile, the full `cty` type system) are tracked
in [ROADMAP.md](ROADMAP.md) and are **not** described below.

---

## 1. Lexical grammar

The lexer (`lex()`) is a single-character-lookahead scanner over a byte buffer.
It produces one token per call; there is **no comment syntax** in the expression
lexer — only ASCII whitespace (`isspace`) separates tokens.

```
NUMBER  = DIGIT { DIGIT | "." | "e" | "E" | exp-sign } ;
          (* a '+' or '-' is consumed only immediately after 'e'/'E';
             the value is parsed with strtod(), so "1", "1.5", "1e9",
             "2.5E-3" are all accepted *)

STRING  = '"' { rawchar | "\" anychar } '"' ;
          (* the lexer captures the RAW inner bytes (escapes kept verbatim)
             and stops at the first unescaped '"'. Escape processing and
             ${ } interpolation happen later, at evaluation — see §4. *)

IDENT   = ID_START { ID_CHAR } ;
ID_START = ALPHA | "_" ;
ID_CHAR  = ALNUM | "_" | "-" ;
          (* the keywords true / false / null are lexed as IDENT and
             recognised in the parser, not by the lexer *)
```

Punctuation / operator tokens:

| Token     | Lexeme | Token    | Lexeme |
|-----------|--------|----------|--------|
| `T_LP`    | `(`    | `T_RP`   | `)`    |
| `T_LB`    | `[`    | `T_RB`   | `]`    |
| `T_LC`    | `{`    | `T_RC`   | `}`    |
| `T_COMMA` | `,`    | `T_DOT`  | `.`    |
| `T_COLON` | `:`    | `T_QUEST`| `?`    |
| `T_ASSIGN`| `=`    | `T_EQ`   | `==`   |
| `T_NE`    | `!=`   | `T_NOT`  | `!`    |
| `T_LT`    | `<`    | `T_LE`   | `<=`   |
| `T_GT`    | `>`    | `T_GE`   | `>=`   |
| `T_PLUS`  | `+`    | `T_MINUS`| `-`    |
| `T_STAR`  | `*`    | `T_SLASH`| `/`    |
| `T_PCT`   | `%`    | `T_AND`  | `&&`   |
| `T_OR`    | `\|\|` |          |        |

`&` not followed by `&`, and `|` not followed by `|`, are lexer errors. Any
other byte that starts none of the above is `T_ERR` ("invalid character").

---

## 2. Syntactic grammar (EBNF)

`{ X }` = zero or more, `[ X ]` = optional. The entry point is `parse_expr`.

```ebnf
expr        = conditional ;

conditional = binary [ "?" expr ":" expr ] ;
              (* the "?:" ternary is the loosest operator; its two branches are
                 full exprs, so it is right-associative and nests freely *)

binary      = unary { binop unary } ;
              (* precedence climbing — see the binding-power table in §3;
                 all binary operators are LEFT-associative *)

unary       = ( "-" | "!" ) unary
            | postfix ;
              (* prefix unary minus / logical-not; right-recursive, so "--x"
                 and "!!x" are accepted *)

postfix     = primary { "." IDENT          (* attribute access  *)
                      | "[" expr "]" } ;    (* index / element   *)

primary     = NUMBER
            | STRING                         (* a template — see §4 *)
            | "true" | "false" | "null"
            | IDENT "(" [ args ] ")"         (* function call *)
            | IDENT                          (* variable reference *)
            | "(" expr ")"                   (* grouping *)
            | tuple
            | object ;

args        = expr { "," expr } [ "," ] ;    (* trailing comma allowed *)

tuple       = "[" [ expr { "," expr } [ "," ] ] "]" ;
              (* comma-separated; trailing comma allowed; may be empty *)

object      = "{" { objitem } "}" ;
objitem     = ( IDENT | STRING ) ( "=" | ":" ) expr [ "," ] ;
              (* both "=" and ":" accepted between key and value;
                 items may be separated by "," OR just whitespace/newline;
                 may be empty *)
```

### Notes on the productions

- **Grouping vs. object** — a `{ ... }` is always an object literal here; there
  is no statement block. A `( ... )` is pure grouping and returns the inner
  expression node directly (no wrapper).
- **Call vs. variable** — disambiguated by one token of lookahead: an `IDENT`
  immediately followed by `(` is a call, otherwise a variable reference.
- **Keywords** — `true`, `false`, `null` are matched by string compare inside
  `primary`; they are otherwise ordinary identifiers, so they cannot be used as
  variable names.
- **Object keys** — bare identifiers or quoted strings. The string-key form is
  parsed but the key is taken literally (template interpolation in keys is not
  evaluated at M1).

---

## 3. Operator precedence (Pratt binding powers)

The expression parser is a **Pratt / precedence-climbing** parser
(`parse_binary(p, minbp)`). Binary operators get a *binding power* from
`binbp()`; the right operand is parsed with `minbp = bp + 1`, which makes every
binary operator **left-associative**.

From **loosest** (binds last) to **tightest** (binds first):

| Precedence | bp  | Operators            | Associativity | Parsed by        |
|-----------:|----:|----------------------|---------------|------------------|
| 1 (loosest)| —   | `? :` (conditional)  | right         | `parse_expr`     |
| 2          | 1   | `\|\|`               | left          | `parse_binary`   |
| 3          | 2   | `&&`                 | left          | `parse_binary`   |
| 4          | 3   | `==`  `!=`           | left          | `parse_binary`   |
| 5          | 4   | `<` `<=` `>` `>=`    | left          | `parse_binary`   |
| 6          | 5   | `+`  `-`             | left          | `parse_binary`   |
| 7          | 6   | `*`  `/`  `%`        | left          | `parse_binary`   |
| 8          | —   | unary `-`  `!`       | right (prefix)| `parse_unary`    |
| 9 (tightest)| —  | `.`  `[ ]`  call `()`| left          | `parse_postfix`/`parse_primary` |

So, for example:

```
a || b && c == d + e * f      parses as   a || (b && (c == (d + (e * f))))
-a.b[c]                        parses as   -((a.b)[c])
!x == y                        parses as   (!x) == y
p ? q : r ? s : t              parses as   p ? q : (r ? s : t)
```

---

## 4. String templates (evaluated, not parsed)

A `STRING` lexeme becomes an `N_TEMPLATE` AST node holding the **raw inner
bytes**. The template is interpreted at evaluation time (`eval_template`), not
during parsing:

- `${ expr }` — the bytes between the braces are re-parsed and evaluated as a
  full expression (with **brace-depth matching**, so `${ {a=1}.a }` works), then
  rendered to text and spliced in. Interpolating a `null`, tuple, or object is
  an error; numbers and booleans render textually.
- `$${` — an escaped literal `${` (no interpolation).
- `%{ ... }` — template **directives** are recognised and **rejected** with a
  clear "not supported yet" error (reserved for a later milestone).
- Backslash escapes (`\n`, `\t`, `\r`, `\"`, `\\`, …) are expanded here.

A plain string with no `${` is just its literal (escaped) text.

---

## 5. AST node kinds

The parser emits these `enum nkind` nodes (`hcl2_internal.h`):

| Node         | Produced by              | Children / payload                         |
|--------------|--------------------------|--------------------------------------------|
| `N_LIT`      | number / `true`/`false`/`null` | `lit` (a `hcl2_value`)              |
| `N_TEMPLATE` | string                   | `str` (raw inner bytes)                    |
| `N_VAR`      | bare identifier          | `str` (name)                               |
| `N_CALL`     | `IDENT(...)`             | `str` (name), `items[ ]` (args)            |
| `N_ATTR`     | `.name`                  | `a` (object), `str` (attr name)            |
| `N_INDEX`    | `[expr]`                 | `a` (collection), `b` (index)              |
| `N_UNARY`    | `-` / `!`                | `op`, `a` (operand)                        |
| `N_BINARY`   | binary operator          | `op`, `a` (left), `b` (right)              |
| `N_COND`     | `? :`                    | `a` (cond), `b` (then), `c` (else)         |
| `N_TUPLE`    | `[ ... ]`                | `items[ ]`                                 |
| `N_OBJECT`   | `{ ... }`                | `keys[ ]`, `items[ ]` (values)             |

---

## 6. Built-in functions (evaluation context)

Resolved by `builtin_func()` after user-supplied context functions. Milestone 1
ships: `length`, `upper`, `lower`, `min`, `max`. Additional functions are
provided by the caller via `hcl2_ctx_set_func`.

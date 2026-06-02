# c-hcl2 roadmap

Honest status of the HCL2 surface. "Full HCL2" is a large language (the Go
reference, hashicorp/hcl v2 + zclconf/go-cty, is tens of thousands of lines);
this is built milestone by milestone.

## M1 — expression engine ✅ (done)

- value model (cty-lite): null, bool, number, string, tuple, object
- lexer + Pratt parser + tree-walking evaluator
- operators: unary `- !`; binary `+ - * / %`, `== != < <= > >=`, `&& ||`
- conditional `cond ? a : b`, parentheses
- tuples `[...]`, objects `{ k = v }` (ident or string keys, `=` or `:`)
- variable references, `.attr` and `[index]` traversal
- function calls; builtins `length`, `upper`, `lower`, `min`, `max`
- string templates with `${ expr }` interpolation (+ `$${` escape)
- evaluation context (variables + user functions)

## M2 — configuration bodies

- top-level body: attributes (`name = expr`) and labeled blocks (`type "l" { }`)
- lazy/decoded evaluation of attribute expressions against a context
- a c-hcl-compatible accessor layer (so c-hcl2 can subsume c-hcl)

## M3 — template & collection expressions

- `for`-expressions: `[for x in xs : f(x)]`, `{for k, v in m : ...}`, `if` filters
- splat: `xs[*].field`
- `%{ for }` / `%{ if }` template directives; heredocs `<<EOF`, `<<-EOF`
- string-index, `...` (variadic) call expansion

## M4 — type system & diagnostics

- the `cty` type system: list/set/map vs tuple/object distinctions,
  type constraints and conversions, **unknown** values
- richer numbers (big.Float semantics) instead of `double`
- source-range diagnostics (line/column spans), multiple errors

## M5 — HCL JSON profile

- parse the JSON representation of HCL bodies/expressions

## Cross-cutting

- allocation fault-injection in tests + raise coverage to ~99% / 100% functions
- fuzzing the lexer/parser

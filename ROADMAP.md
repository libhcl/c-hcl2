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

## M2 — configuration bodies ✅ (done)

- top-level body: attributes (`name = expr`) and labeled blocks (`type "l" { }`),
  arbitrarily nested
- lazy/decoded evaluation of attribute expressions against a context
  (`hcl2_body_attr_value`) — the same document decodes against different bindings
- a c-hcl-compatible accessor layer (`hcl2_doc`/`hcl2_body`/`hcl2_block`,
  `hcl2_doc_root`, `hcl2_body_attr_*`, `hcl2_body_block_count`/`_at`,
  `hcl2_block_type`/`_label`/`_body`) — so c-hcl2 can subsume c-hcl
- `#`, `//`, and `/* */` comments in the lexer (benefits expressions too)

  *Not yet:* newline-significant attribute termination (an attribute value is the
  longest expression that parses); revisit with M4 diagnostics.

## M3 — template & collection expressions ✅ (done)

- ✅ `for`-expressions: tuple `[for x in xs : f(x)]`, object
  `{for k, v in m : k => v}`, the `k, v` key/index variable, and `if` filters
- ✅ splat: `xs[*].field` (desugars to a for-expression). *Not yet:* index
  trailers after a splat (`xs[*][0]`) or chained splats (`xs[*].a[*]`).
- ✅ heredocs `<<EOF` and indented `<<-EOF` (common-leading-whitespace strip);
  the body is a template (honours `${ }`) but keeps backslashes literal
- ✅ `%{ if }` / `%{ else }` / `%{ endif }` and `%{ for }` / `%{ endfor }`
  template directives (nestable; `$${` and `%%{` escapes)
- ✅ `...` (variadic) call expansion: `f(a, xs...)` spreads a tuple's elements
  as the trailing arguments. (String indexing is not part of HCL — strings are
  scalars in the `cty`-lite model — so it is intentionally absent.)

## M4 — type system & diagnostics

- the `cty` type system: list/set/map vs tuple/object distinctions,
  type constraints and conversions, **unknown** values
- richer numbers (big.Float semantics) instead of `double`
- source-range diagnostics (line/column spans), multiple errors

## M5 — HCL JSON profile

- parse the JSON representation of HCL bodies/expressions

## Cross-cutting

- ✅ allocation fault-injection in tests (`HCL2_FAULT_INJECT` budget hook in
  `hcl2_alloc.h`, shared by all TUs) + an OOM-scan harness: **100% functions**,
  ~90% lines, clean under ASan on every injected out-of-memory path
- ⬜ fuzzing the lexer/parser
- ⬜ push line coverage further (the remaining ~10% are deep OOM/error edges)

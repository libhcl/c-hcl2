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

## M4 — type system & diagnostics (started)

- 🟡 the `cty` type system: **type constraints + conversion done**
  (`hcl2_type_*` + `hcl2_convert`: primitive coercions, and list/set/map/any as
  constraints — list/set normalise to a homogeneous tuple, map to an object,
  set de-duplicates). **Unknown values done** (`hcl2_unknown` / the
  `HCL2_UNKNOWN` kind): operations propagate unknown through binary/unary/
  conditional/index/attribute/call/for-expression and template interpolation +
  `%{ if }`/`%{ for }` directives; convert(unknown) is unknown. *Not yet:*
  distinct list/set/map runtime kinds, the tuple-vs-list distinction, and
  type-tracked unknowns (the unknown here is fully dynamic).
- ⬜ richer numbers (big.Float semantics) instead of `double`
- 🟡 source-range diagnostics: both syntax (lex/parse) **and** semantic/eval
  errors now report `at line L, column C`. AST nodes carry the position
  (computed at parse time), so eval errors are located even for **deferred body
  decoding**, after the source buffer is gone. *Not yet:* full ranges (spans,
  not just a start point) and collecting multiple errors per parse.

## M5 — HCL JSON profile (started)

- 🟡 value layer done: `hcl2_parse_json` parses a JSON document into the value
  model (object/array/string/number/bool/null, with `\uXXXX` -> UTF-8). *Not
  yet:* the schema-driven body profile (attribute-vs-block resolution, JSON
  strings decoded as HCL templates), which needs the decode-with-schema work.

## Cross-cutting

- ✅ allocation fault-injection in tests (`HCL2_FAULT_INJECT` budget hook in
  `hcl2_alloc.h`, shared by all TUs) + an OOM-scan harness: **100% functions**,
  ~90% lines, clean under ASan on every injected out-of-memory path
- ✅ fuzzing the lexer/parser: `make fuzz` (deterministic, fixed-seed; random /
  token-soup / seed-mutation inputs into hcl2_eval + hcl2_parse, exact-length
  buffers so ASan catches over-reads). Clean over millions of iterations across
  several seeds.
- ⬜ push line coverage further (the remaining ~10% are deep OOM/error edges)

## HCL2 native-syntax compliance (tracking the gaps to 100%)

The goal is full HCL2 native-syntax compatibility. Known remaining gaps, roughly
by impact:

- ✅ string escapes `\uNNNN` / `\UNNNNNNNN` (UTF-8) — done
- ✅ template whitespace trimming `${~ ~}` / `%{~ ~}` (strip the adjacent run of
  whitespace, on interpolations and all directives) — done
- ✅ legacy attribute-only splat `list.*.attr` — done
- ✅ a splat now captures the whole following relative traversal — `.attr` and
  `[index]` trailers (`xs[*].a[0]`) map per element, matching HCL. *Still
  unsupported:* chained splats (`xs[*][*]`, `.*.* `), rejected with a clear error
- ✅ object-`for` grouping mode `{for ... : k => v...}` (same-key values collected
  into a tuple) — done
- ⬜ distinct cty collection kinds (list/set/map vs tuple/object) + the
  tuple-vs-list distinction; type-tracked unknowns
- ⬜ arbitrary-precision numbers (cty uses big.Float; we use `double`)
- ⬜ full source ranges (start+end spans) and multi-error reporting
- ⬜ the JSON profile's schema-driven body layer

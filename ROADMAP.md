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
  (`hcl2_type_*` + `hcl2_convert`). **Distinct collection kinds done**:
  `HCL2_LIST` / `HCL2_SET` / `HCL2_MAP` are real value kinds (a list/set is not
  equal to a tuple, a map is not an object). `hcl2_convert` to list/set/map
  produces them; they flow through indexing, `length`, `for`-expressions,
  `jsonencode`, etc. (list/set share tuple storage, map shares object storage).
  **Unknown values done** (`hcl2_unknown` / `HCL2_UNKNOWN`): operations
  propagate unknown through binary/unary/conditional/index/attribute/call/
  for-expression and template interpolation + directives.
  **Typed unknowns done**: an unknown can carry the cty type it stands for
  (`hcl2_unknown_of(type)`, queried via `hcl2_unknown_type`); `hcl2_convert`
  refines an unknown to its target type (converting to `any` is the identity and
  preserves the carried type). `hcl2_unknown()` remains a fully dynamic
  ("any"-typed) unknown. *Not yet:* eval-level type refinement -- unknowns
  produced *by operations* (e.g. `unknown + 1`) are still dynamic rather than
  carrying the inferred result type.
- ⬜ richer numbers (big.Float semantics) instead of `double`
- 🟡 source-range diagnostics: both syntax (lex/parse) **and** semantic/eval
  errors report `at line L, column C`. AST nodes carry the position (computed at
  parse time), so eval errors are located even for **deferred body decoding**,
  after the source buffer is gone. **Multi-error collection done**:
  `hcl2_parse_diags` recovers at the next line and gathers all body-level
  errors into a `hcl2_diags` list (a best-effort partial document is still
  returned). *Not yet:* full ranges (start+end spans, not just a start point).

## M5 — HCL JSON profile (started)

- ✅ value layer done: `hcl2_parse_json` parses a JSON document into the value
  model (object/array/string/number/bool/null, with `\uXXXX` -> UTF-8).
- ✅ expression/template decoding done: `hcl2_json_eval` evaluates a JSON
  document where each JSON string is an HCL template (so `"${var}"` and
  `"%{ if c }..%{ endif }"` expand against a context; backslashes stay literal
  since JSON already un-escaped). Objects/arrays map to object/tuple of
  evaluated values; unknown interpolations propagate unknown. Reuses the native
  template renderer (`eval_template`, heredoc mode).
- 🟡 *Not yet:* the schema-driven **body** profile -- using a schema to resolve
  which JSON properties are attributes vs. (labeled) blocks, so a JSON document
  can be decoded as an `hcl2_body`. This needs the decode-with-schema work; the
  value/expression layers above are the pieces it will build on.

## Cross-cutting

- ✅ allocation fault-injection in tests (`HCL2_FAULT_INJECT` budget hook in
  `hcl2_alloc.h`, shared by all TUs) + an OOM-scan harness: **100% functions**,
  ~90% lines, clean under ASan on every injected out-of-memory path
- ✅ fuzzing the lexer/parser: `make fuzz` (deterministic, fixed-seed; random /
  token-soup / seed-mutation inputs into hcl2_eval + hcl2_parse, exact-length
  buffers so ASan catches over-reads). Clean over millions of iterations across
  several seeds.
- 🟡 line coverage raised to ~99% (100% functions). Exhaustive-switch dead
  `return`s were removed by making the last case a `default:`; the residual ~1%
  is the deepest nested allocation-failure cleanup arms and a few defensive NULL
  guards. (Literal 100% lines isn't cleanly reachable in C without deleting
  defensive code.)

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
- ✅ nested strings inside interpolation (`"${ f("x") }"`) — the string lexer is
  interpolation-aware (tracks `${ }`/`%{ }` depth and nested quotes)
- ✅ distinct cty collection kinds (list/set/map vs tuple/object) + the
  tuple-vs-list distinction — done
- ✅ type-tracked (typed) unknowns — done (`hcl2_unknown_of` / `hcl2_unknown_type`;
  `hcl2_convert` refines). *Still:* eval-level inference of result types onto
  operation-produced unknowns
- ✅ multi-error reporting (`hcl2_parse_diags`) — done. *Still:* full source
  ranges (start+end spans, not just a start point)
- ✅ JSON profile value + expression/template decoding (`hcl2_parse_json`,
  `hcl2_json_eval`) — done. *Still:* the schema-driven **body** layer
  (attribute-vs-block resolution from a schema)
- ⬜ arbitrary-precision numbers (cty uses big.Float; we use `double` — a
  deliberate scope decision; see below)

## Deliberate scope decisions

- **Numbers are IEEE-754 `double`, not arbitrary-precision `big.Float`.** cty
  models numbers as arbitrary-precision decimals; c-hcl2 uses `double`. This is
  a conscious trade-off: a `double` carries ~15-17 significant decimal digits,
  which covers configuration values (ports, counts, ratios, timeouts) with room
  to spare, and keeps the value model dependency-free and fast. Programs that
  need exact decimal arithmetic on very large integers or high-precision
  fractions are out of scope. Revisiting this would mean vendoring or depending
  on a bignum library, which conflicts with the zero-dependency goal.
- **The remaining true gap to full parity is the JSON profile's schema-driven
  *body* layer** (decoding a JSON document into an `hcl2_body` by consulting a
  schema to tell attributes from blocks). The native-syntax surface, the cty
  value semantics, typed unknowns, conversions, multi-error diagnostics, and the
  JSON value + expression/template layers are all implemented.

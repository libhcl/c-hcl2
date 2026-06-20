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
- ✅ splat: `xs[*].field` (desugars to a for-expression), index trailers after a
  splat (`xs[*][0]`), **and chained splats** (`xs[*][*]`, `a.*.*`) — a further
  splat wraps the per-element body in a nested splat.
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
  ("any"-typed) unknown. **Eval-level type inference done**: unknowns produced
  *by operations* now carry the inferred result type -- arithmetic ->
  `unknown(number)`, comparisons/`==`/`!=`/`&&`/`||` -> `unknown(bool)`, unary
  `!`/`-`, and a template with an unknown interpolation -> `unknown(string)`
  (index/attribute/conditional/call results stay dynamic, where the type cannot
  be inferred without deeper machinery).
- ⬜ richer numbers (big.Float semantics) instead of `double`
- 🟡 source-range diagnostics: both syntax (lex/parse) **and** semantic/eval
  errors report `at line L, column C`. AST nodes carry the position (computed at
  parse time), so eval errors are located even for **deferred body decoding**,
  after the source buffer is gone. **Multi-error collection done**:
  `hcl2_parse_diags` recovers at the next line and gathers all body-level
  errors into a `hcl2_diags` list (a best-effort partial document is still
  returned). **Full source spans done**: every AST node now carries an end
  position (exclusive) alongside its start, exposed by the public
  `hcl2_expr_span()` (parse one expression, report its start+end without
  evaluating); the lexer tracks the previous token's end and composite nodes
  inherit their rightmost child's end.

## M5 — HCL JSON profile (started)

- ✅ value layer done: `hcl2_parse_json` parses a JSON document into the value
  model (object/array/string/number/bool/null, with `\uXXXX` -> UTF-8).
- ✅ expression/template decoding done: `hcl2_json_eval` evaluates a JSON
  document where each JSON string is an HCL template (so `"${var}"` and
  `"%{ if c }..%{ endif }"` expand against a context; backslashes stay literal
  since JSON already un-escaped). Objects/arrays map to object/tuple of
  evaluated values; unknown interpolations propagate unknown. Reuses the native
  template renderer (`eval_template`, heredoc mode).
- ✅ schema-driven **body** profile done: `hcl2_json_decode(src, len, schema,
  ...)` decodes a JSON document into the same `hcl2_doc`/`hcl2_body` tree the
  native parser builds, using a `hcl2_schema` (`hcl2_schema_new` /
  `hcl2_schema_attr` / `hcl2_schema_block`) to resolve attributes vs. labeled
  blocks. Attribute values are synthesized into expression AST nodes (JSON
  strings -> templates), so they decode **lazily** against a context via
  `hcl2_body_attr_value`, exactly like native bodies; blocks descend `nlabels`
  deep (a leaf object is one block, an array is several sharing labels). Missing
  required attributes, unknown properties, and label-shape mismatches are
  errors. With this, the JSON profile is complete (value + expression + body).

## M6 — Terraform function library ✅ (done)

The full Terraform `lang/funcs` set (~97 functions), one function per file under
`builtin/`, each validated against Terraform's own test vectors. By category:

- **string**: `upper` `lower` `title` `substr` `strrev` `replace` (literal + regex)
  `trim*` `chomp` `indent` `join` `split` `startswith` `endswith` `format`
  `formatlist` …
- **number**: `abs` `ceil` `floor` `signum` `log` `pow` `parseint` `min` `max` …
- **collection/sequence/set**: `concat` `element` `slice` `reverse` `sum` `range`
  `sort` `distinct` `compact` `flatten` `index` `one` `alltrue` `anytrue`
  `coalesce(list)` `merge` `zipmap` `chunklist` `matchkeys` `keys` `values`
  `lookup` `contains` `setunion`/`setintersection`/`setsubtract`/`setproduct`
  `transpose` …
- **conversion**: `tostring` `tonumber` `tobool` `tolist` `toset` `tomap`
  `jsonencode` `jsondecode` `csvdecode`
- **regex** (`regex`, `regexall`): a self-contained RE2-subset engine (bytecode +
  backtracking VM, classes, groups/named groups, quantifiers, alternation; no
  backreferences/lookaround, matching RE2)
- **crypto/encoding**: `base64encode`/`decode` `base64sha256`/`512` `md5` `sha1`
  `sha256` `sha512` `uuidv5` `uuid` (pure-C reference digests, validated against
  standard vectors)
- **datetime**: `timestamp` `plantimestamp` `timeadd` `timecmp` `formatdate`
- **network**: `cidrhost` `cidrnetmask` `cidrsubnet` `cidrsubnets` (pure
  IPv4/IPv6 address math)
- **filesystem**: `file` `fileexists` `filebase64` `fileset` (doublestar glob)
  `templatefile` (renders a file as an HCL template) `abspath` `dirname`
  `basename` `pathexpand`

*Deliberate exclusions* (cannot be byte-compatible with Terraform from an
independent implementation, the same class as the `big.Float` decision):
`base64gzip` (Go `compress/flate`-specific output), `bcrypt` (random salt). The
glob engine handles `*`/`?`/`[..]`/`**`/`{a,b}`, and `yamldecode` handles block
scalars (`|`/`>` with `-`/`+` chomping), node anchors/aliases (`&a`/`*a`),
core-schema tags (`!!str`/`!!int`/…), merge keys (`<<`) and multi-document
streams (`---`/`...`, decoded to a tuple of documents — a superset of Terraform,
which accepts only one). `yamlencode` emits byte-exact, sorted-key block output.

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
- ✅ a splat captures the whole following relative traversal — `.attr` and
  `[index]` trailers (`xs[*].a[0]`) map per element, matching HCL — **and chained
  splats** (`xs[*][*]`, `a.*.*`) now parse and evaluate as nested splats
- ✅ object-`for` grouping mode `{for ... : k => v...}` (same-key values collected
  into a tuple) — done
- ✅ nested strings inside interpolation (`"${ f("x") }"`) — the string lexer is
  interpolation-aware (tracks `${ }`/`%{ }` depth and nested quotes)
- ✅ distinct cty collection kinds (list/set/map vs tuple/object) + the
  tuple-vs-list distinction — done
- ✅ type-tracked (typed) unknowns — done (`hcl2_unknown_of` / `hcl2_unknown_type`;
  `hcl2_convert` refines) — **plus eval-level inference** of result types onto
  operation-produced unknowns
- ✅ multi-error reporting (`hcl2_parse_diags`) — done — **plus full source
  ranges** (start+end spans via `hcl2_expr_span`)
- ✅ the Terraform `lang/funcs` library (~97 functions) — done (see M6)
- ✅ JSON profile — value (`hcl2_parse_json`), expression/template
  (`hcl2_json_eval`), **and** the schema-driven body layer (`hcl2_json_decode` +
  `hcl2_schema_*`) — all done
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
- **The HCL2 feature surface is implemented end to end:** native syntax, cty
  value semantics, typed unknowns with eval-level result-type inference,
  conversions, multi-error diagnostics with full source ranges, chained splats,
  the full JSON profile (value + expression/template + schema-driven body), and
  the Terraform `lang/funcs` library (M6). The only remaining trade-off is the
  `big.Float` precision decision documented above (plus the deliberate
  function exclusions noted in M6).

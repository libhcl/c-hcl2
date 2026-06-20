<p align="center"><img src="https://raw.githubusercontent.com/libhcl/brand/main/social/libhcl.png" alt="libhcl/c-hcl2" width="720"></p>

# c-hcl2

A from-scratch C implementation of **HCL2** — the heavyweight companion to
[libhcl/c-hcl](https://github.com/libhcl/c-hcl) (which parses only the
declarative subset). Use c-hcl when you want a tiny config reader; use c-hcl2
when you need the **expression language**.

> **Status: feature-complete.** The HCL2 native syntax (expressions, bodies,
> templates, for/splat/heredocs/directives/spread), a cty-lite value model with
> distinct collection kinds and typed unknowns + eval-level type inference,
> type constraints + conversion, multi-error diagnostics with full source spans,
> the complete JSON profile (value + expression + schema-driven body), and the
> **Terraform `lang/funcs` library (~97 functions)** are all implemented and
> tested. The only deliberate trade-offs are `double` numbers (not `big.Float`)
> and a few functions that can't be byte-compatible from an independent
> implementation; see [ROADMAP.md](ROADMAP.md).

## What works now

**Expressions** (`hcl2_eval`): numbers, booleans, `null`, quoted-string
**templates** with `${ expr }` interpolation, tuples `[...]`, objects
`{ k = v, ... }`, unary `- !`, binary `+ - * / %`, comparison `== != < <= > >=`,
logical `&& ||`, the conditional `cond ? a : b`, parentheses, variable
references with `.attr` / `[index]` traversal, function calls — evaluated
against a context of variables and functions — plus **for-expressions**
(`[for x in xs : x*2 if x>0]`, `{for k, v in m : k => v}`), **splat**
(`xs[*].name`, chained `xs[*][*]`), **heredocs** (`<<EOF` / indented `<<-EOF`),
**template directives** (`%{ if }`/`%{ else }`/`%{ endif }`,
`%{ for }`/`%{ endfor }`), **variadic spread** (`max(xs...)`), and the lazy
special forms `try(...)` / `can(...)` for graceful optional access.

**Terraform function library** — the full `lang/funcs` set (~97 functions), each
validated against Terraform's own test vectors and living one-per-file under
[`builtin/`](builtin/): string (`upper`/`substr`/`replace`/`format`/`join`/…),
number (`abs`/`ceil`/`log`/`pow`/`parseint`/…), collection/set
(`concat`/`flatten`/`merge`/`zipmap`/`setunion`/`transpose`/…), conversion
(`to*`, `jsonencode`/`jsondecode`, `csvdecode`), regex (`regex`/`regexall`, a
self-contained RE2-subset engine), crypto/encoding
(`base64*`/`md5`/`sha*`/`uuidv5`/`uuid`), datetime
(`timestamp`/`timeadd`/`timecmp`/`formatdate`), network
(`cidrhost`/`cidrnetmask`/`cidrsubnet`/`cidrsubnets`), filesystem
(`file`/`fileset`/`templatefile`/`abspath`/`dirname`/…), and
`yamldecode`/`yamlencode`. (`base64gzip`/`bcrypt` are deliberately excluded —
they can't be byte-compatible from an independent implementation; see
[ROADMAP.md](ROADMAP.md).)

**Configuration bodies** (`hcl2_parse`): documents of attributes
(`name = expr`) and nested, optionally labeled blocks (`type "label" { ... }`),
with `#` / `//` / `/* */` comments. Attribute expressions are decoded **lazily**
against a context (`hcl2_body_attr_value`), and the c-hcl-style accessors
(`hcl2_doc_root`, `hcl2_body_block_count`/`_at`, `hcl2_block_type`/`_label`/
`_body`) let c-hcl2 subsume [c-hcl](https://github.com/libhcl/c-hcl).

```c
hcl2_doc *doc = hcl2_parse(src, len, err, sizeof err);
const hcl2_body *root = hcl2_doc_root(doc);
hcl2_value *port = hcl2_body_attr_value(root, "port", ctx, err, sizeof err);
const hcl2_block *svc = hcl2_body_block_at(root, "service", 0);
```

**Unknown values** (`hcl2_unknown` / typed `hcl2_unknown_of`) model cty's
plan-time placeholders: an operation touching an unknown (arithmetic,
comparison, conditionals, traversal, calls, for-expressions, template
interpolation/directives) propagates unknown, and where the result type is
determined by the operation alone the produced unknown carries it — arithmetic →
`unknown(number)`, comparisons/logic → `unknown(bool)`, a template with an
unknown interpolation → `unknown(string)` (queried via `hcl2_unknown_type`).

A **type-constraint / conversion** layer (`hcl2_type_*` + `hcl2_convert`) coerces
values toward a target type — primitive coercions (number↔string, string→bool, …)
and `list`/`set`/`map`/`any` constraints. The distinct cty collection kinds
(`HCL2_LIST`/`HCL2_SET`/`HCL2_MAP`, separate from tuple/object) are real value
kinds that flow through indexing, `length`, `for`-expressions and `jsonencode`.

**Source spans**: every syntax and eval diagnostic reports `at line L, column C`,
and `hcl2_expr_span()` parses a single expression and reports its full start+end
span (end exclusive) without evaluating it; `hcl2_parse_diags` collects all
body-level errors at once.

`hcl2_parse_json` reads a JSON document into the same value model (object→object,
array→tuple, scalars map directly, `\uXXXX`→UTF-8) — the value layer of HCL's
JSON profile; the schema-driven body profile is future work.

The exact grammar that is parsed today — lexical tokens, EBNF productions (both
expressions and bodies), the Pratt operator-precedence table, and the template
rules — is documented in [GRAMMAR.md](GRAMMAR.md).

```c
#include "hcl2.h"

hcl2_ctx *ctx = hcl2_ctx_new();
hcl2_ctx_set_var(ctx, "port", hcl2_number(8080));
hcl2_ctx_set_var(ctx, "name", hcl2_string("api"));

char err[256];
hcl2_value *v = hcl2_eval("\"${name}:${port + 1}\"", 0 /*len*/, ctx, err, sizeof err);
//                          ^ pass strlen(src) as len
// v == "api:8081"

hcl2_value_free(v);
hcl2_ctx_free(ctx);
```

(See [`hcl2.h`](hcl2.h) for the full value/context API.)

## Build & test

```sh
make            # builds libhcl2.a
make test       # unit tests (add SANITIZE=address on a system clang)
make cover      # llvm-cov report
```

No toolchain? With [pkgx](https://pkgx.sh): `dev` (reads pkgx.yaml) or `./taskw test`.

The whole surface is unit-tested under AddressSanitizer — expressions,
bodies/blocks, templates and directives, traversal/splat, the JSON profile, the
type/conversion and unknown-inference layers, and every Terraform function
against its Terraform test vectors — alongside an allocation fault-injection
scan (`HCL2_FAULT_INJECT`, 100% functions) and a fixed-seed fuzzer
(`make fuzz`), clean over millions of iterations.

## License

BSD-3-Clause. See [LICENSE](LICENSE).

# c-hcl2

A from-scratch C implementation of **HCL2** — the heavyweight companion to
[libhcl/c-hcl](https://github.com/libhcl/c-hcl) (which parses only the
declarative subset). Use c-hcl when you want a tiny config reader; use c-hcl2
when you need the **expression language**.

> **Status: milestones 1–2 — expression engine + configuration bodies.** This
> is *not* yet a spec-complete HCL2. What works today is the HCL2 expression
> sub-language, a cty-lite value model, and document bodies (attributes +
> labeled blocks) with lazy decoding against a context; what's missing is
> tracked in [ROADMAP.md](ROADMAP.md) (heredocs, `%{ }` template directives,
> the JSON profile, the full `cty` type system with unknowns, and source-range
> diagnostics). It is honest about being a work in progress.

## What works now

**Expressions** (`hcl2_eval`): numbers, booleans, `null`, quoted-string
**templates** with `${ expr }` interpolation, tuples `[...]`, objects
`{ k = v, ... }`, unary `- !`, binary `+ - * / %`, comparison `== != < <= > >=`,
logical `&& ||`, the conditional `cond ? a : b`, parentheses, variable
references with `.attr` / `[index]` traversal, function calls — evaluated
against a context of variables and functions (builtins: `length`, `upper`,
`lower`, `min`, `max`) — plus **for-expressions** (`[for x in xs : x*2 if x>0]`,
`{for k, v in m : k => v}`), **splat** (`xs[*].name`), **heredocs**
(`<<EOF` / indented `<<-EOF`), and **template directives**
(`%{ if }`/`%{ else }`/`%{ endif }`, `%{ for }`/`%{ endfor }`).

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

The evaluator is unit-tested under AddressSanitizer (arithmetic, precedence,
templates, tuples/objects, traversal, functions, variables, and ~11 error
cases). Coverage of the implemented surface will be raised toward the
~99%/100%-function bar of the sibling libraries as the milestones land.

## License

BSD-3-Clause. See [LICENSE](LICENSE).

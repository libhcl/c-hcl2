# c-hcl2

A from-scratch C implementation of **HCL2** — the heavyweight companion to
[libhcl/c-hcl](https://github.com/libhcl/c-hcl) (which parses only the
declarative subset). Use c-hcl when you want a tiny config reader; use c-hcl2
when you need the **expression language**.

> **Status: milestone 1 — the expression engine.** This is *not* yet a
> spec-complete HCL2. What works today is the HCL2 expression sub-language and a
> cty-lite value model; what's missing is tracked in [ROADMAP.md](ROADMAP.md)
> (heredocs, `for`-expressions, splat, `%{ }` template directives, the JSON
> profile, the full `cty` type system with unknowns, and source-range
> diagnostics). It is honest about being a work in progress.

## What works now

Numbers, booleans, `null`, quoted-string **templates** with `${ expr }`
interpolation, tuples `[...]`, objects `{ k = v, ... }`, unary `- !`, binary
`+ - * / %`, comparison `== != < <= > >=`, logical `&& ||`, the conditional
`cond ? a : b`, parentheses, variable references with `.attr` / `[index]`
traversal, and function calls — evaluated against a context of variables and
functions (builtins: `length`, `upper`, `lower`, `min`, `max`).

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

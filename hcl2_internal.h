#ifndef C_HCL2_INTERNAL_H
#define C_HCL2_INTERNAL_H

/* Private declarations shared between the c-hcl2 translation units
 * (value.c <-> hcl2.c). Not part of the public API. */
#include "hcl2.h"

struct kv {
  char *key;
  hcl2_value *val;
};
struct hcl2_value {
  hcl2_kind kind;
  bool b;
  double num;
  char *str;
  hcl2_value **items; /* tuple */
  size_t n;
  struct kv *fields; /* object */
  size_t nf;
};

/* value.c internals used by the evaluator (hcl2.c) */
hcl2_value *vclone(const hcl2_value *v);
bool vequal(const hcl2_value *a, const hcl2_value *b);
const hcl2_value *ctx_var(hcl2_ctx *c, const char *name);
hcl2_func ctx_func(hcl2_ctx *c, const char *name);
hcl2_func builtin_func(const char *name);

/* error reporter (defined in hcl2.c, used by the builtins in value.c too) */
void everr(char *err, size_t errsz, const char *m);

#endif /* C_HCL2_INTERNAL_H */

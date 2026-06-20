#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* installed in the inner context so a template cannot call templatefile() on
 * itself (matching Terraform's recursion prohibition). */
static hcl2_value *tf_guard(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  (void)a;
  (void)n;
  everr(e, es, "templatefile() cannot be called recursively");
  return NULL;
}

hcl2_value *bi_templatefile(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || !hcl2_is_keyed(a[1]->kind)) {
    everr(e, es, "templatefile() needs (path, vars object)");
    return NULL;
  }
  size_t len;
  char *buf = read_file(a[0]->str, &len, e, es);
  if (buf == NULL)
    return NULL;
  if (!utf8_valid(buf, len)) {
    free(buf);
    everr(e, es, "templatefile() contents are not valid UTF-8");
    return NULL;
  }
  hcl2_ctx *ctx = hcl2_ctx_new();
  if (ctx == NULL) {
    free(buf);
    return NULL;
  }
  bool ok = hcl2_ctx_set_func(ctx, "templatefile", tf_guard);
  for (size_t i = 0; ok && i < a[1]->nf; i++) {
    hcl2_value *cv = vclone(a[1]->fields[i].val);
    if (cv == NULL || !hcl2_ctx_set_var(ctx, a[1]->fields[i].key, cv)) {
      hcl2_value_free(cv);
      ok = false;
    }
  }
  hcl2_value *r = ok ? eval_template(buf, true, ctx, e, es) : NULL;
  hcl2_ctx_free(ctx);
  free(buf);
  return r;
}

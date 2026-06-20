#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_zipmap(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || !hcl2_is_seq(a[0]->kind) || !hcl2_is_seq(a[1]->kind)) {
    everr(e, es, "zipmap() needs (keys, values)");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  if (L != hcl2_value_len(a[1])) {
    everr(e, es, "zipmap() keys and values must have equal length");
    return NULL;
  }
  hcl2_value *out = hcl2_object();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < L; i++) {
    const hcl2_value *k = hcl2_value_at(a[0], i);
    if (k->kind != HCL2_STRING) {
      everr(e, es, "zipmap() keys must be strings");
      hcl2_value_free(out);
      return NULL;
    }
    hcl2_value *v = vclone(hcl2_value_at(a[1], i));
    if (v == NULL || !hcl2_object_set(out, k->str, v)) {
      hcl2_value_free(v);
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

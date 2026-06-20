#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* ---- map / object functions (Terraform) ---- */
hcl2_value *bi_merge(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  hcl2_value *out = hcl2_object();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    if (!hcl2_is_keyed(a[i]->kind)) {
      everr(e, es, "merge() arguments must be objects or maps");
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t j = 0; j < a[i]->nf; j++) {
      hcl2_value *v = vclone(a[i]->fields[j].val);
      if (v == NULL || !hcl2_object_set(out, a[i]->fields[j].key, v)) {
        hcl2_value_free(v);
        hcl2_value_free(out);
        return NULL;
      }
    }
  }
  return out;
}

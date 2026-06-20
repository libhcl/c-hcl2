#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

static bool seen(hcl2_value *out, const hcl2_value *el) {
  for (size_t k = 0; k < hcl2_value_len(out); k++)
    if (vequal(hcl2_value_at(out, k), el))
      return true;
  return false;
}
hcl2_value *bi_setunion(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n < 1) {
    everr(e, es, "setunion() needs at least one set");
    return NULL;
  }
  hcl2_value *out = hcl2_set();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    if (!hcl2_is_seq(a[i]->kind)) {
      everr(e, es, "setunion() arguments must be sets or lists");
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t j = 0; j < hcl2_value_len(a[i]); j++) {
      const hcl2_value *el = hcl2_value_at(a[i], j);
      if (!seen(out, el) && !push_clone(out, el)) {
        hcl2_value_free(out);
        return NULL;
      }
    }
  }
  return out;
}

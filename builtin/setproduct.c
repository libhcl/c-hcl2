#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_setproduct(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n < 1) {
    everr(e, es, "setproduct() needs at least one argument");
    return NULL;
  }
  size_t total = 1;
  for (size_t i = 0; i < n; i++) {
    if (!hcl2_is_seq(a[i]->kind)) {
      everr(e, es, "setproduct() arguments must be sets or lists");
      return NULL;
    }
    size_t L = hcl2_value_len(a[i]);
    if (L == 0)
      return hcl2_tuple();
    total *= L;
    if (total > 1000000) {
      everr(e, es, "setproduct() result is too large");
      return NULL;
    }
  }
  size_t *idx = calloc(n ? n : 1, sizeof *idx);
  if (idx == NULL)
    return NULL;
  hcl2_value *out = hcl2_tuple();
  if (out == NULL) {
    free(idx);
    return NULL;
  }
  for (size_t c = 0; c < total; c++) {
    hcl2_value *combo = hcl2_tuple();
    if (combo == NULL) {
      free(idx);
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t i = 0; i < n; i++)
      if (!push_clone(combo, hcl2_value_at(a[i], idx[i]))) {
        hcl2_value_free(combo);
        free(idx);
        hcl2_value_free(out);
        return NULL;
      }
    if (!hcl2_tuple_push(out, combo)) {
      hcl2_value_free(combo);
      free(idx);
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t i = n; i-- > 0;) {
      if (++idx[i] < hcl2_value_len(a[i]))
        break;
      idx[i] = 0;
    }
  }
  free(idx);
  return out;
}

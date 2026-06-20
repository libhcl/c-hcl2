#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* ---- collection ---- */
hcl2_value *bi_concat(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    if (!hcl2_is_seq(a[i]->kind)) {
      everr(e, es, "concat() needs tuples");
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t j = 0; j < a[i]->n; j++) {
      hcl2_value *c = vclone(a[i]->items[j]);
      if (c == NULL || !hcl2_tuple_push(out, c)) {
        hcl2_value_free(c);
        hcl2_value_free(out);
        return NULL;
      }
    }
  }
  return out;
}

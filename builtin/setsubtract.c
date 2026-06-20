#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

static bool member(const hcl2_value *set, const hcl2_value *el) {
  for (size_t k = 0; k < hcl2_value_len(set); k++)
    if (vequal(hcl2_value_at(set, k), el))
      return true;
  return false;
}
hcl2_value *bi_setsubtract(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || !hcl2_is_seq(a[0]->kind) || !hcl2_is_seq(a[1]->kind)) {
    everr(e, es, "setsubtract() needs (a, b) sets or lists");
    return NULL;
  }
  hcl2_value *out = hcl2_set();
  if (out == NULL)
    return NULL;
  for (size_t j = 0; j < hcl2_value_len(a[0]); j++) {
    const hcl2_value *el = hcl2_value_at(a[0], j);
    if (!member(a[1], el) && !member(out, el) && !push_clone(out, el)) {
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

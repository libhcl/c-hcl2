#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_matchkeys(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 3 || !hcl2_is_seq(a[0]->kind) || !hcl2_is_seq(a[1]->kind) || !hcl2_is_seq(a[2]->kind)) {
    everr(e, es, "matchkeys() needs (values, keys, searchset)");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  if (L != hcl2_value_len(a[1])) {
    everr(e, es, "matchkeys() values and keys must have equal length");
    return NULL;
  }
  size_t S = hcl2_value_len(a[2]);
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < L; i++) {
    const hcl2_value *k = hcl2_value_at(a[1], i);
    bool match = false;
    for (size_t j = 0; j < S; j++)
      if (vequal(hcl2_value_at(a[2], j), k)) {
        match = true;
        break;
      }
    if (match && !push_clone(out, hcl2_value_at(a[0], i))) {
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

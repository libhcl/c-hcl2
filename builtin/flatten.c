#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_flatten(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "flatten() needs a list");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  if (!flatten_into(out, a[0])) {
    hcl2_value_free(out);
    return NULL;
  }
  return out;
}

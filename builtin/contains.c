#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_contains(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "contains() needs (tuple, value)");
    return NULL;
  }
  for (size_t i = 0; i < a[0]->n; i++)
    if (vequal(a[0]->items[i], a[1]))
      return hcl2_bool(true);
  return hcl2_bool(false);
}

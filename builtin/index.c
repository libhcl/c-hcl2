#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_index(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "index() needs (list, value)");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  for (size_t i = 0; i < L; i++)
    if (vequal(hcl2_value_at(a[0], i), a[1]))
      return hcl2_number((double)i);
  everr(e, es, "index() item not found in list");
  return NULL;
}

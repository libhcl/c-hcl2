#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_one(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "one() needs a list");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  if (L == 0)
    return hcl2_null();
  if (L == 1)
    return vclone(hcl2_value_at(a[0], 0));
  everr(e, es, "one() requires a list of zero or one elements");
  return NULL;
}

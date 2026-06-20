#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_sum(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "sum() needs a list of numbers");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  if (L == 0) {
    everr(e, es, "sum() cannot use an empty list");
    return NULL;
  }
  double s = 0;
  for (size_t i = 0; i < L; i++) {
    const hcl2_value *el = hcl2_value_at(a[0], i);
    if (el->kind != HCL2_NUMBER) {
      everr(e, es, "sum() list must contain only numbers");
      return NULL;
    }
    s += el->num;
  }
  return hcl2_number(s);
}

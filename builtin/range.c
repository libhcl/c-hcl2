#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_range(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n < 1 || n > 3) {
    everr(e, es, "range() takes 1 to 3 numbers");
    return NULL;
  }
  for (size_t i = 0; i < n; i++)
    if (a[i]->kind != HCL2_NUMBER) {
      everr(e, es, "range() arguments must be numbers");
      return NULL;
    }
  double start = 0, limit, step = 1;
  if (n == 1) {
    limit = a[0]->num;
  } else {
    start = a[0]->num;
    limit = a[1]->num;
    if (n == 3)
      step = a[2]->num;
  }
  if (step == 0) {
    everr(e, es, "range() step must be non-zero");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (double v = start; (step > 0) ? (v < limit) : (v > limit); v += step) {
    hcl2_value *num = hcl2_number(v);
    if (num == NULL || !hcl2_tuple_push(out, num)) {
      hcl2_value_free(num);
      hcl2_value_free(out);
      return NULL;
    }
    if (hcl2_value_len(out) > 1000000) {
      everr(e, es, "range() produces too many values");
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

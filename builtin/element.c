#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_element(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || !hcl2_is_seq(a[0]->kind) || a[1]->kind != HCL2_NUMBER) {
    everr(e, es, "element() needs (list, index)");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  if (L == 0) {
    everr(e, es, "element() cannot use an empty list");
    return NULL;
  }
  if (a[1]->num < 0) {
    everr(e, es, "element() index cannot be negative");
    return NULL;
  }
  return vclone(hcl2_value_at(a[0], (size_t)a[1]->num % L));
}

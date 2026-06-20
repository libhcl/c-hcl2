#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_slice(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 3 || !hcl2_is_seq(a[0]->kind) || a[1]->kind != HCL2_NUMBER ||
      a[2]->kind != HCL2_NUMBER) {
    everr(e, es, "slice() needs (list, start, end)");
    return NULL;
  }
  long L = (long)hcl2_value_len(a[0]);
  long from = (long)a[1]->num, to = (long)a[2]->num;
  if (from < 0 || to > L || from > to) {
    everr(e, es, "slice() index out of range");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (long i = from; i < to; i++)
    if (!push_clone(out, hcl2_value_at(a[0], (size_t)i))) {
      hcl2_value_free(out);
      return NULL;
    }
  return out;
}

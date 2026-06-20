#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_trim(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "trim() needs (string, cutset)");
    return NULL;
  }
  const char *s = a[0]->str, *cut = a[1]->str;
  size_t lo = 0, hi = strlen(s);
  while (lo < hi && in_cutset(s[lo], cut))
    lo++;
  while (hi > lo && in_cutset(s[hi - 1], cut))
    hi--;
  return mkstr_n(s + lo, hi - lo);
}

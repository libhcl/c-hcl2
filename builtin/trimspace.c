#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_trimspace(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "trimspace() needs one string");
    return NULL;
  }
  const char *s = a[0]->str;
  size_t lo = 0, hi = strlen(s);
  while (lo < hi && isspace((unsigned char)s[lo]))
    lo++;
  while (hi > lo && isspace((unsigned char)s[hi - 1]))
    hi--;
  return mkstr_n(s + lo, hi - lo);
}

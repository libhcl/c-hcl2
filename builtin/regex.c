#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_regex(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "regex() needs (pattern, string)");
    return NULL;
  }
  hregex *re = hre_compile(a[0]->str, e, es);
  if (re == NULL)
    return NULL;
  const char *s = a[1]->str;
  size_t len = strlen(s);
  int *cap = calloc((size_t)(2 * (hre_ngroups(re) + 1)), sizeof *cap);
  if (cap == NULL) {
    hre_free(re);
    return NULL;
  }
  hcl2_value *r = NULL;
  if (hre_match(re, s, len, 0, cap))
    r = hre_submatch_value(re, s, cap, e, es);
  else
    everr(e, es, "regex: no match found");
  free(cap);
  hre_free(re);
  return r;
}

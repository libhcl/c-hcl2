#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_regexall(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "regexall() needs (pattern, string)");
    return NULL;
  }
  hregex *re = hre_compile(a[0]->str, e, es);
  if (re == NULL)
    return NULL;
  const char *s = a[1]->str;
  size_t len = strlen(s);
  int *cap = calloc((size_t)(2 * (hre_ngroups(re) + 1)), sizeof *cap);
  hcl2_value *out = hcl2_list();
  if (cap == NULL || out == NULL) {
    free(cap);
    hcl2_value_free(out);
    hre_free(re);
    return NULL;
  }
  size_t from = 0;
  while (from <= len && hre_match(re, s, len, from, cap)) {
    hcl2_value *m = hre_submatch_value(re, s, cap, e, es);
    if (m == NULL || !hcl2_tuple_push(out, m)) {
      hcl2_value_free(m);
      free(cap);
      hcl2_value_free(out);
      hre_free(re);
      return NULL;
    }
    size_t end = (size_t)cap[1];
    from = (end > (size_t)cap[0]) ? end : end + 1;
  }
  free(cap);
  hre_free(re);
  return out;
}

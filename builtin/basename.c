#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_basename(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "basename() needs (path)");
    return NULL;
  }
  const char *p = a[0]->str;
  if (*p == '\0')
    return hcl2_string(".");
  size_t L = strlen(p);
  while (L > 0 && p[L - 1] == '/') /* strip trailing slashes */
    L--;
  if (L == 0)
    return hcl2_string("/"); /* path was all slashes */
  size_t start = L;
  while (start > 0 && p[start - 1] != '/')
    start--;
  return mkstr_n(p + start, L - start);
}

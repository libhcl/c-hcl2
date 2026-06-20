#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_trimprefix(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "trimprefix() needs (string, prefix)");
    return NULL;
  }
  const char *s = a[0]->str, *p = a[1]->str;
  size_t pl = strlen(p);
  if (strncmp(s, p, pl) == 0)
    return hcl2_string(s + pl);
  return hcl2_string(s);
}

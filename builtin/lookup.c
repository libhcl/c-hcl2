#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_lookup(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 3 || !hcl2_is_keyed(a[0]->kind) || a[1]->kind != HCL2_STRING) {
    everr(e, es, "lookup() needs (object, string, default)");
    return NULL;
  }
  const hcl2_value *f = hcl2_value_get(a[0], a[1]->str);
  return vclone(f ? f : a[2]);
}

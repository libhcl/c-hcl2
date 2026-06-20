#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_tomap(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1) {
    everr(e, es, "tomap() takes 1 argument");
    return NULL;
  }
  hcl2_type *t = hcl2_type_map(hcl2_type_any());
  hcl2_value *r = hcl2_convert(a[0], t, e, es);
  hcl2_type_free(t);
  return r;
}

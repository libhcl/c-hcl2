#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_coalesce(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  for (size_t i = 0; i < n; i++)
    if (a[i]->kind != HCL2_NULL)
      return vclone(a[i]);
  everr(e, es, "coalesce() received no non-null argument");
  return NULL;
}

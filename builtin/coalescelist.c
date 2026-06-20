#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_coalescelist(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  for (size_t i = 0; i < n; i++) {
    if (!hcl2_is_seq(a[i]->kind)) {
      everr(e, es, "coalescelist() arguments must be lists");
      return NULL;
    }
    if (hcl2_value_len(a[i]) > 0)
      return vclone(a[i]);
  }
  everr(e, es, "coalescelist() requires at least one non-empty list");
  return NULL;
}

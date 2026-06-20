#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_jsonencode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1) {
    everr(e, es, "jsonencode() takes 1 argument");
    return NULL;
  }
  struct sb s = {0};
  if (!json_emit(a[0], &s)) { /* json_emit only fails on OOM (-> NULL, like elsewhere) */
    free(s.p);
    return NULL;
  }
  hcl2_value *v = hcl2_string(s.p ? s.p : "");
  free(s.p);
  return v;
}

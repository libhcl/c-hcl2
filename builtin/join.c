#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_join(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || !hcl2_is_seq(a[1]->kind)) {
    everr(e, es, "join() needs (string, tuple)");
    return NULL;
  }
  struct sb s = {0};
  for (size_t i = 0; i < a[1]->n; i++) {
    if (a[1]->items[i]->kind != HCL2_STRING) {
      everr(e, es, "join() tuple elements must be strings");
      free(s.p);
      return NULL;
    }
    if (i > 0)
      sb_puts(&s, a[0]->str);
    sb_puts(&s, a[1]->items[i]->str);
  }
  if (s.oom) {
    free(s.p);
    return NULL;
  }
  hcl2_value *v = hcl2_string(s.p ? s.p : "");
  free(s.p);
  return v;
}

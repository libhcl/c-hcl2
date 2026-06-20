#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_indent(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_NUMBER || a[1]->kind != HCL2_STRING) {
    everr(e, es, "indent() needs (number, string)");
    return NULL;
  }
  int pad = (int)a[0]->num;
  if (pad < 0)
    pad = 0;
  const char *s = a[1]->str;
  struct sb b = {0};
  for (const char *p = s; *p; p++) {
    sb_put(&b, p, 1);
    if (*p == '\n') {
      for (int i = 0; i < pad; i++)
        sb_put(&b, " ", 1);
    }
  }
  if (b.oom) {
    free(b.p);
    return NULL;
  }
  hcl2_value *r = hcl2_string(b.p ? b.p : "");
  free(b.p);
  return r;
}

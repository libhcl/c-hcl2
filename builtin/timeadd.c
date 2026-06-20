#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_timeadd(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "timeadd() needs (timestamp, duration)");
    return NULL;
  }
  long long epoch, secs;
  int off;
  if (!rfc3339_parse(a[0]->str, &epoch, &off, e, es))
    return NULL;
  if (!duration_parse(a[1]->str, &secs, e, es))
    return NULL;
  char *s = rfc3339_format(epoch + secs, off);
  if (s == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(s);
  free(s);
  return r;
}

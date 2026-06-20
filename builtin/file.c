#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_file(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "file() needs (path)");
    return NULL;
  }
  size_t len;
  char *buf = read_file(a[0]->str, &len, e, es);
  if (buf == NULL)
    return NULL;
  if (!utf8_valid(buf, len)) {
    free(buf);
    everr(e, es, "file() contents are not valid UTF-8");
    return NULL;
  }
  hcl2_value *r = mkstr_n(buf, len);
  free(buf);
  return r;
}

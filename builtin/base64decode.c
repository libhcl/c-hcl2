#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_base64decode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "base64decode() needs one string");
    return NULL;
  }
  size_t outlen;
  unsigned char *d = hc_base64_decode(a[0]->str, &outlen);
  if (d == NULL) {
    everr(e, es, "base64decode() invalid base64");
    return NULL;
  }
  hcl2_value *r = mkstr_n((const char *)d, outlen);
  free(d);
  return r;
}

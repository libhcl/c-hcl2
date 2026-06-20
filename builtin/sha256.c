#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_sha256(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "sha256() needs one string");
    return NULL;
  }
  unsigned char h[32];
  hc_sha256((const unsigned char *)a[0]->str, strlen(a[0]->str), h);
  char *hx = hc_hex(h, 32);
  if (hx == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(hx);
  free(hx);
  return r;
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_base64sha512(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "base64sha512() needs one string");
    return NULL;
  }
  unsigned char h[64];
  hc_sha512((const unsigned char *)a[0]->str, strlen(a[0]->str), h);
  char *b = hc_base64_encode(h, 64);
  if (b == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(b);
  free(b);
  return r;
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_strrev(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "strrev() needs one string");
    return NULL;
  }
  const char *s = a[0]->str;
  size_t len = strlen(s);
  char *out = malloc(len + 1);
  if (out == NULL)
    return NULL;
  out[len] = '\0';
  size_t w = len;
  for (const char *p = s; *p;) {
    size_t cl = u8_clen((unsigned char)*p);
    if (cl > (size_t)(s + len - p))
      cl = 1;
    w -= cl;
    memcpy(out + w, p, cl);
    p += cl;
  }
  hcl2_value *r = hcl2_string(out);
  free(out);
  return r;
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_filebase64(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "filebase64() needs (path)");
    return NULL;
  }
  size_t len;
  char *buf = read_file(a[0]->str, &len, e, es);
  if (buf == NULL)
    return NULL;
  char *b64 = hc_base64_encode((const unsigned char *)buf, len);
  free(buf);
  if (b64 == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(b64);
  free(b64);
  return r;
}

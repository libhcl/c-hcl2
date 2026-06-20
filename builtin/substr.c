#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_substr(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 3 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_NUMBER ||
      a[2]->kind != HCL2_NUMBER) {
    everr(e, es, "substr() needs (string, offset, length)");
    return NULL;
  }
  const char *s = a[0]->str;
  long R = (long)u8_runes(s);
  long off = (long)a[1]->num;
  long len = (long)a[2]->num;
  if (off < 0)
    off += R;
  if (off < 0)
    off = 0;
  if (off > R)
    off = R;
  long end = (len < 0) ? R : off + len;
  if (end > R)
    end = R;
  if (end < off)
    end = off;
  size_t b0 = u8_byteoff(s, (size_t)off);
  size_t b1 = u8_byteoff(s, (size_t)end);
  return mkstr_n(s + b0, b1 - b0);
}

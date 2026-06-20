#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* formatlist: like format, but list arguments are iterated element-wise
 * (scalars are repeated), producing a list of formatted strings. */
hcl2_value *bi_formatlist(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n < 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "formatlist() needs a format string");
    return NULL;
  }
  long len = -1;
  for (size_t i = 1; i < n; i++)
    if (hcl2_is_seq(a[i]->kind)) {
      long L = (long)hcl2_value_len(a[i]);
      if (len < 0)
        len = L;
      else if (len != L) {
        everr(e, es, "formatlist() list arguments must have the same length");
        return NULL;
      }
    }
  if (len < 0)
    len = 1;
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  const hcl2_value **sub = malloc(n * sizeof *sub + 1);
  if (sub == NULL) {
    hcl2_value_free(out);
    return NULL;
  }
  sub[0] = a[0];
  for (long idx = 0; idx < len; idx++) {
    for (size_t i = 1; i < n; i++)
      sub[i] = hcl2_is_seq(a[i]->kind) ? hcl2_value_at(a[i], (size_t)idx) : a[i];
    hcl2_value *r = bi_format(sub, n, e, es);
    if (r == NULL || !hcl2_tuple_push(out, r)) {
      hcl2_value_free(r);
      free(sub);
      hcl2_value_free(out);
      return NULL;
    }
  }
  free(sub);
  return out;
}

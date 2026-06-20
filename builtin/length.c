#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* ---- string ---- */
hcl2_value *bi_length(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1) {
    everr(e, es, "length() takes 1 argument");
    return NULL;
  }
  const hcl2_value *v = a[0];
  if (v->kind == HCL2_STRING)
    return hcl2_number((double)u8_runes(v->str));
  if (hcl2_is_seq(v->kind) || hcl2_is_keyed(v->kind))
    return hcl2_number((double)hcl2_value_len(v));
  everr(e, es, "length() needs a string, tuple or object");
  return NULL;
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_compact(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "compact() needs a list of strings");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < L; i++) {
    const hcl2_value *el = hcl2_value_at(a[0], i);
    if (el->kind != HCL2_STRING) {
      everr(e, es, "compact() list must contain only strings");
      hcl2_value_free(out);
      return NULL;
    }
    if (el->str[0] != '\0' && !push_clone(out, el)) {
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

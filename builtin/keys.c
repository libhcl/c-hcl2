#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_keys(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_keyed(a[0]->kind)) {
    everr(e, es, "keys() needs an object");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < a[0]->nf; i++) {
    hcl2_value *k = hcl2_string(a[0]->fields[i].key);
    if (k == NULL || !hcl2_tuple_push(out, k)) {
      hcl2_value_free(k);
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_reverse(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "reverse() needs a list");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = hcl2_value_len(a[0]); i-- > 0;)
    if (!push_clone(out, hcl2_value_at(a[0], i))) {
      hcl2_value_free(out);
      return NULL;
    }
  return out;
}

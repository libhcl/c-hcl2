#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_chunklist(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || !hcl2_is_seq(a[0]->kind) || a[1]->kind != HCL2_NUMBER) {
    everr(e, es, "chunklist() needs (list, size)");
    return NULL;
  }
  long sz = (long)a[1]->num;
  if (sz < 0) {
    everr(e, es, "chunklist() size cannot be negative");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  size_t step = (sz == 0) ? (L ? L : 1) : (size_t)sz;
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < L; i += step) {
    hcl2_value *chunk = hcl2_tuple();
    if (chunk == NULL) {
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t j = i; j < i + step && j < L; j++)
      if (!push_clone(chunk, hcl2_value_at(a[0], j))) {
        hcl2_value_free(chunk);
        hcl2_value_free(out);
        return NULL;
      }
    if (!hcl2_tuple_push(out, chunk)) {
      hcl2_value_free(chunk);
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_sort(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "sort() needs a list of strings");
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  for (size_t i = 0; i < L; i++)
    if (hcl2_value_at(a[0], i)->kind != HCL2_STRING) {
      everr(e, es, "sort() list must contain only strings");
      return NULL;
    }
  const hcl2_value **arr = malloc(L * sizeof *arr + 1);
  if (arr == NULL)
    return NULL;
  for (size_t i = 0; i < L; i++)
    arr[i] = hcl2_value_at(a[0], i);
  qsort(arr, L, sizeof *arr, sort_cmp);
  hcl2_value *out = hcl2_tuple();
  if (out == NULL) {
    free(arr);
    return NULL;
  }
  for (size_t i = 0; i < L; i++)
    if (!push_clone(out, arr[i])) {
      free(arr);
      hcl2_value_free(out);
      return NULL;
    }
  free(arr);
  return out;
}

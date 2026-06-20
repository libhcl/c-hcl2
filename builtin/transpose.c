#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_transpose(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_keyed(a[0]->kind)) {
    everr(e, es, "transpose() needs a map of lists of strings");
    return NULL;
  }
  hcl2_value *out = hcl2_object();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < a[0]->nf; i++) {
    const char *K = a[0]->fields[i].key;
    const hcl2_value *lst = a[0]->fields[i].val;
    if (!hcl2_is_seq(lst->kind)) {
      everr(e, es, "transpose() values must be lists of strings");
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t j = 0; j < hcl2_value_len(lst); j++) {
      const hcl2_value *s = hcl2_value_at(lst, j);
      if (s->kind != HCL2_STRING) {
        everr(e, es, "transpose() values must be lists of strings");
        hcl2_value_free(out);
        return NULL;
      }
      hcl2_value *bucket = NULL;
      for (size_t k = 0; k < out->nf; k++)
        if (strcmp(out->fields[k].key, s->str) == 0) {
          bucket = out->fields[k].val;
          break;
        }
      if (bucket == NULL) {
        bucket = hcl2_tuple();
        if (bucket == NULL || !hcl2_object_set(out, s->str, bucket)) {
          hcl2_value_free(bucket);
          hcl2_value_free(out);
          return NULL;
        }
      }
      bool have = false;
      for (size_t k = 0; k < hcl2_value_len(bucket); k++)
        if (strcmp(hcl2_value_at(bucket, k)->str, K) == 0) {
          have = true;
          break;
        }
      if (!have) {
        hcl2_value *kv = hcl2_string(K);
        if (kv == NULL || !hcl2_tuple_push(bucket, kv)) {
          hcl2_value_free(kv);
          hcl2_value_free(out);
          return NULL;
        }
      }
    }
  }
  for (size_t k = 0; k < out->nf; k++) {
    hcl2_value *bucket = out->fields[k].val;
    qsort(bucket->items, hcl2_value_len(bucket), sizeof(hcl2_value *), sort_cmp);
  }
  return out;
}

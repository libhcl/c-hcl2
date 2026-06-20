#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_split(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "split() needs (string, string)");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  const char *sep = a[0]->str, *str = a[1]->str;
  if (*sep == '\0') { /* empty separator -> the whole string as one element */
    hcl2_value *s = hcl2_string(str);
    if (s == NULL || !hcl2_tuple_push(out, s)) {
      hcl2_value_free(s);
      hcl2_value_free(out);
      return NULL;
    }
    return out;
  }
  size_t seplen = strlen(sep);
  const char *p = str;
  for (;;) {
    const char *hit = strstr(p, sep);
    size_t seglen = hit ? (size_t)(hit - p) : strlen(p);
    char *seg = malloc(seglen + 1);
    if (seg == NULL) {
      hcl2_value_free(out);
      return NULL;
    }
    memcpy(seg, p, seglen);
    seg[seglen] = '\0';
    hcl2_value *sv = hcl2_string(seg);
    free(seg);
    if (sv == NULL || !hcl2_tuple_push(out, sv)) {
      hcl2_value_free(sv);
      hcl2_value_free(out);
      return NULL;
    }
    if (hit == NULL)
      return out;
    p = hit + seplen;
  }
}

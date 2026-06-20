#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_parseint(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_NUMBER) {
    everr(e, es, "parseint() needs (string, base)");
    return NULL;
  }
  double bd = a[1]->num;
  int base = (int)bd;
  if (bd != (double)base || base < 2 || base > 62) {
    everr(e, es, "parseint() base must be an integer in 2..62");
    return NULL;
  }
  const char *s = a[0]->str;
  bool neg = false;
  if (*s == '+' || *s == '-') {
    neg = (*s == '-');
    s++;
  }
  if (*s == '\0') {
    everr(e, es, "parseint() cannot parse empty string as an integer");
    return NULL;
  }
  double v = 0;
  for (; *s; s++) {
    int d = parseint_digit(*s, base);
    if (d < 0) {
      everr(e, es, "parseint() invalid digit for the given base");
      return NULL;
    }
    v = v * (double)base + (double)d;
  }
  return hcl2_number(neg ? -v : v);
}

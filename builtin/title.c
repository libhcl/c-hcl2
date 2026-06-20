#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_title(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "title() needs one string");
    return NULL;
  }
  hcl2_value *r = hcl2_string(a[0]->str);
  if (r == NULL)
    return NULL;
  bool prev_letter = false;
  for (char *c = r->str; *c; c++) {
    bool letter = isalpha((unsigned char)*c) != 0;
    if (letter && !prev_letter)
      *c = (char)toupper((unsigned char)*c);
    prev_letter = letter;
  }
  return r;
}

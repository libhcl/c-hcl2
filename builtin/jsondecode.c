#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* ---- JSON ---- */
hcl2_value *bi_jsondecode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "jsondecode() needs a string");
    return NULL;
  }
  return hcl2_parse_json(a[0]->str, strlen(a[0]->str), e, es);
}

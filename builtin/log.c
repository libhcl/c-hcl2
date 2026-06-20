#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_log(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_NUMBER || a[1]->kind != HCL2_NUMBER) {
    everr(e, es, "log() needs (number, base)");
    return NULL;
  }
  return hcl2_number(log(a[0]->num) / log(a[1]->num));
}

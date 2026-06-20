#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_signum(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_NUMBER) {
    everr(e, es, "signum() needs one number");
    return NULL;
  }
  double x = a[0]->num;
  return hcl2_number(x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0));
}

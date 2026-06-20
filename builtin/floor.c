#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_floor(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return num1(a, n, e, es, floor, "floor() needs one number");
}

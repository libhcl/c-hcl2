#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_timecmp(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "timecmp() needs (timestamp_a, timestamp_b)");
    return NULL;
  }
  long long ea, eb;
  int oa, ob;
  if (!rfc3339_parse(a[0]->str, &ea, &oa, e, es) || !rfc3339_parse(a[1]->str, &eb, &ob, e, es))
    return NULL;
  return hcl2_number(ea < eb ? -1 : (ea > eb ? 1 : 0));
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* In Terraform plantimestamp() is the plan-time clock; with no plan context it
 * behaves like timestamp() (current UTC time in RFC 3339). */
hcl2_value *bi_plantimestamp(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  (void)a;
  if (n != 0) {
    everr(e, es, "plantimestamp() takes no arguments");
    return NULL;
  }
  char *s = rfc3339_format((long long)time(NULL), 0);
  if (s == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(s);
  free(s);
  return r;
}

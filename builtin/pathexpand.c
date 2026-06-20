#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_pathexpand(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "pathexpand() needs (path)");
    return NULL;
  }
  const char *p = a[0]->str;
  if (p[0] != '~')
    return hcl2_string(p);
  if (p[1] != '\0' && p[1] != '/') {
    everr(e, es, "pathexpand() cannot expand a user-specific home directory");
    return NULL;
  }
  const char *home = getenv("HOME");
  if (home == NULL || *home == '\0') {
    everr(e, es, "pathexpand() cannot determine the home directory");
    return NULL;
  }
  size_t need = strlen(home) + strlen(p + 1) + 1;
  char *res = malloc(need);
  if (res == NULL)
    return NULL;
  snprintf(res, need, "%s%s", home, p + 1);
  hcl2_value *r = hcl2_string(res);
  free(res);
  return r;
}

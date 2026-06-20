#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_dirname(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "dirname() needs (path)");
    return NULL;
  }
  const char *p = a[0]->str;
  const char *slash = strrchr(p, '/');
  char *dir;
  if (slash == NULL) {
    dir = strdup("");
  } else {
    size_t L = (size_t)(slash - p) + 1; /* keep the trailing slash, Clean drops it */
    dir = malloc(L + 1);
    if (dir != NULL) {
      memcpy(dir, p, L);
      dir[L] = '\0';
    }
  }
  if (dir == NULL)
    return NULL;
  char *cleaned = clean_path(dir);
  free(dir);
  if (cleaned == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(cleaned);
  free(cleaned);
  return r;
}

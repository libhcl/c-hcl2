#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_abspath(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "abspath() needs (path)");
    return NULL;
  }
  const char *p = a[0]->str;
  char *cleaned;
  if (p[0] == '/') {
    cleaned = clean_path(p);
  } else {
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd) == NULL) {
      everr(e, es, "abspath() cannot determine the working directory");
      return NULL;
    }
    size_t need = strlen(cwd) + 1 + strlen(p) + 1;
    char *joined = malloc(need);
    if (joined == NULL)
      return NULL;
    snprintf(joined, need, "%s/%s", cwd, p);
    cleaned = clean_path(joined);
    free(joined);
  }
  if (cleaned == NULL)
    return NULL;
  hcl2_value *r = hcl2_string(cleaned);
  free(cleaned);
  return r;
}

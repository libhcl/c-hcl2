#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

hcl2_value *bi_fileexists(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "fileexists() needs (path)");
    return NULL;
  }
  struct stat st;
  if (stat(a[0]->str, &st) != 0) {
    if (errno == ENOENT)
      return hcl2_bool(false);
    everr(e, es, "fileexists() cannot stat path");
    return NULL;
  }
  if (S_ISDIR(st.st_mode)) {
    everr(e, es, "fileexists() path is a directory, not a file");
    return NULL;
  }
  return hcl2_bool(S_ISREG(st.st_mode));
}

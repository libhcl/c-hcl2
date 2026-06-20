#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* recursively collect regular files under base, relative path `rel`, whose
 * '/'-joined relative path matches the glob pattern. */
static bool walk(const char *base, const char *rel, const char *pat, hcl2_value *out) {
  char dir[4096];
  if (rel[0] == '\0')
    snprintf(dir, sizeof dir, "%s", base);
  else
    snprintf(dir, sizeof dir, "%s/%s", base, rel);
  DIR *d = opendir(dir);
  if (d == NULL)
    return true; /* unreadable subdirectory: skip quietly */
  bool ok = true;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    char childrel[4096], full[4096];
    if (rel[0] == '\0')
      snprintf(childrel, sizeof childrel, "%s", de->d_name);
    else
      snprintf(childrel, sizeof childrel, "%s/%s", rel, de->d_name);
    snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
    struct stat st;
    if (stat(full, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      if (!walk(base, childrel, pat, out)) {
        ok = false;
        break;
      }
    } else if (S_ISREG(st.st_mode) && glob_match(pat, childrel)) {
      hcl2_value *v = hcl2_string(childrel);
      if (v == NULL || !hcl2_tuple_push(out, v)) {
        hcl2_value_free(v);
        ok = false;
        break;
      }
    }
  }
  closedir(d);
  return ok;
}

hcl2_value *bi_fileset(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "fileset() needs (path, pattern)");
    return NULL;
  }
  const char *base = a[0]->str;
  if (*base == '\0')
    base = ".";
  struct stat st;
  if (stat(base, &st) != 0 || !S_ISDIR(st.st_mode)) {
    everr(e, es, "fileset() base path is not a directory");
    return NULL;
  }
  hcl2_value *tup = hcl2_tuple();
  if (tup == NULL)
    return NULL;
  if (!walk(base, "", a[1]->str, tup)) {
    hcl2_value_free(tup);
    return NULL;
  }
  hcl2_type *t = hcl2_type_set(hcl2_type_any());
  hcl2_value *set = hcl2_convert(tup, t, e, es);
  hcl2_type_free(t);
  hcl2_value_free(tup);
  return set;
}

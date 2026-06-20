#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* expand a replacement template ($0, $1, ${name}, $$ -> $) against a match */
static void expand_repl(struct sb *b, const char *tmpl, const hregex *re, const char *s,
                        const int *cap) {
  int ng = hre_ngroups(re);
  for (const char *p = tmpl; *p;) {
    if (*p != '$') {
      sb_put(b, p, 1);
      p++;
      continue;
    }
    p++;
    if (*p == '$') {
      sb_put(b, "$", 1);
      p++;
      continue;
    }
    bool brace = (*p == '{');
    if (brace)
      p++;
    char nm[64];
    int ni = 0;
    if (isdigit((unsigned char)*p))
      while (isdigit((unsigned char)*p) && ni < 63)
        nm[ni++] = *p++;
    else
      while ((isalnum((unsigned char)*p) || *p == '_') && ni < 63)
        nm[ni++] = *p++;
    nm[ni] = '\0';
    if (brace && *p == '}')
      p++;
    if (ni == 0) {
      sb_put(b, "$", 1);
      continue;
    }
    int g = -1;
    if (isdigit((unsigned char)nm[0])) {
      g = atoi(nm);
    } else {
      for (int k = 1; k <= ng; k++) {
        const char *gn = hre_group_name(re, k);
        if (gn && strcmp(gn, nm) == 0) {
          g = k;
          break;
        }
      }
    }
    if (g >= 0 && g <= ng && cap[2 * g] >= 0 && cap[2 * g + 1] >= 0)
      sb_put(b, s + cap[2 * g], (size_t)(cap[2 * g + 1] - cap[2 * g]));
  }
}

hcl2_value *bi_replace(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 3 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING ||
      a[2]->kind != HCL2_STRING) {
    everr(e, es, "replace() needs (string, substr, replacement)");
    return NULL;
  }
  const char *s = a[0]->str, *old = a[1]->str, *neu = a[2]->str;
  size_t oldlen = strlen(old);
  /* /pattern/ form: regular-expression replace with $-group expansion */
  if (oldlen >= 2 && old[0] == '/' && old[oldlen - 1] == '/') {
    char *inner = malloc(oldlen - 1);
    if (inner == NULL)
      return NULL;
    memcpy(inner, old + 1, oldlen - 2);
    inner[oldlen - 2] = '\0';
    hregex *re = hre_compile(inner, e, es);
    free(inner);
    if (re == NULL)
      return NULL;
    int *cap = calloc((size_t)(2 * (hre_ngroups(re) + 1)), sizeof *cap);
    if (cap == NULL) {
      hre_free(re);
      return NULL;
    }
    struct sb b = {0};
    size_t len = strlen(s), from = 0, last = 0;
    while (from <= len && hre_match(re, s, len, from, cap)) {
      size_t ms = (size_t)cap[0], me = (size_t)cap[1];
      sb_put(&b, s + last, ms - last);
      expand_repl(&b, neu, re, s, cap);
      last = me;
      from = (me > ms) ? me : me + 1;
    }
    sb_put(&b, s + last, strlen(s + last));
    free(cap);
    hre_free(re);
    if (b.oom) {
      free(b.p);
      return NULL;
    }
    hcl2_value *r = hcl2_string(b.p ? b.p : "");
    free(b.p);
    return r;
  }
  struct sb b = {0};
  if (oldlen == 0) {
    /* Go strings.ReplaceAll: insert before each rune and at the end */
    sb_puts(&b, neu);
    for (const char *p = s; *p;) {
      size_t cl = u8_clen((unsigned char)*p);
      sb_put(&b, p, cl);
      p += cl;
      sb_puts(&b, neu);
    }
  } else {
    const char *p = s;
    for (;;) {
      const char *hit = strstr(p, old);
      if (hit == NULL) {
        sb_puts(&b, p);
        break;
      }
      sb_put(&b, p, (size_t)(hit - p));
      sb_puts(&b, neu);
      p = hit + oldlen;
    }
  }
  if (b.oom) {
    free(b.p);
    return NULL;
  }
  hcl2_value *r = hcl2_string(b.p ? b.p : "");
  free(b.p);
  return r;
}

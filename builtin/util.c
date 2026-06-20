#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* ===========================================================================
 * Builtin functions
 *
 * Each takes already-evaluated arguments and returns a fresh owned value, or
 * NULL on error (message via everr). Unknown propagation is handled by the
 * caller (an unknown argument makes the whole call unknown), so builtins never
 * see HCL2_UNKNOWN arguments.
 * ===========================================================================*/

/* ---- a tiny growable string buffer, for join() and jsonencode() ---- */

bool sb_put(struct sb *s, const char *d, size_t n) {
  if (s->oom)
    return false;
  if (s->len + n + 1 > s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 64;
    while (cap < s->len + n + 1)
      cap *= 2;
    char *np = realloc(s->p, cap);
    if (np == NULL) {
      s->oom = true;
      return false;
    }
    s->p = np;
    s->cap = cap;
  }
  memcpy(s->p + s->len, d, n);
  s->len += n;
  s->p[s->len] = '\0';
  return true;
}

bool sb_puts(struct sb *s, const char *str) { return sb_put(s, str, strlen(str)); }

/* ---- UTF-8 helpers (Terraform strings are sequences of unicode codepoints,
 * so length/substr/strrev count characters, not bytes) ---- */
size_t u8_clen(unsigned char c) {
  if (c < 0x80)
    return 1;
  if ((c & 0xE0) == 0xC0)
    return 2;
  if ((c & 0xF0) == 0xE0)
    return 3;
  if ((c & 0xF8) == 0xF0)
    return 4;
  return 1; /* invalid lead byte: treat as a single byte */
}

size_t u8_runes(const char *s) {
  size_t n = 0;
  for (const char *p = s; *p;) {
    p += u8_clen((unsigned char)*p);
    n++;
  }
  return n;
}

/* byte offset of the r-th rune (clamped to the string's end) */
size_t u8_byteoff(const char *s, size_t r) {
  const char *p = s;
  for (size_t i = 0; i < r && *p; i++)
    p += u8_clen((unsigned char)*p);
  return (size_t)(p - s);
}

hcl2_value *str1(const hcl2_value *const *a, size_t n, char *e, size_t es, bool up) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "upper()/lower() need one string");
    return NULL;
  }
  hcl2_value *r = hcl2_string(a[0]->str);
  if (r == NULL)
    return NULL;
  for (char *c = r->str; *c; c++)
    *c = (char)(up ? toupper((unsigned char)*c) : tolower((unsigned char)*c));
  return r;
}

/* ---- numeric ---- */
hcl2_value *minmax(const hcl2_value *const *a, size_t n, char *e, size_t es, bool mx) {
  if (n == 0) {
    everr(e, es, "min()/max() need at least one number");
    return NULL;
  }
  double r = 0;
  for (size_t i = 0; i < n; i++) {
    if (a[i]->kind != HCL2_NUMBER) {
      everr(e, es, "min()/max() need numbers");
      return NULL;
    }
    if (i == 0 || (mx ? a[i]->num > r : a[i]->num < r))
      r = a[i]->num;
  }
  return hcl2_number(r);
}

hcl2_value *num1(const hcl2_value *const *a, size_t n, char *e, size_t es, double (*f)(double),
                 const char *who) {
  if (n != 1 || a[0]->kind != HCL2_NUMBER) {
    everr(e, es, who);
    return NULL;
  }
  return hcl2_number(f(a[0]->num));
}

/* big.Int.SetString digit rules: base<=36 treats letters case-insensitively
 * (a-z,A-Z = 10..35); base>36 uses a-z = 10..35 and A-Z = 36..61. */
int parseint_digit(char c, int base) {
  int d;
  if (c >= '0' && c <= '9')
    d = c - '0';
  else if (c >= 'a' && c <= 'z')
    d = c - 'a' + 10;
  else if (c >= 'A' && c <= 'Z')
    d = (base <= 36) ? (c - 'A' + 10) : (c - 'A' + 36);
  else
    return -1;
  return d >= base ? -1 : d;
}

/* ---- more string functions (Terraform) ---- */
/* build an owned string value from a (ptr,len) byte slice */
hcl2_value *mkstr_n(const char *p, size_t len) {
  char *tmp = malloc(len + 1);
  if (tmp == NULL)
    return NULL;
  memcpy(tmp, p, len);
  tmp[len] = '\0';
  hcl2_value *v = hcl2_string(tmp);
  free(tmp);
  return v;
}

bool in_cutset(char c, const char *cut) { return strchr(cut, c) != NULL && c != '\0'; }

/* ---- type conversion (wrap hcl2_convert) ---- */
hcl2_value *conv1(const hcl2_value *const *a, size_t n, char *e, size_t es, hcl2_type *t,
                  const char *who) {
  if (n != 1) {
    everr(e, es, who);
    return NULL;
  }
  return hcl2_convert(a[0], t, e, es); /* t is a singleton: no free needed */
}

bool json_emit(const hcl2_value *v, struct sb *s) {
  char buf[40];
  switch (v->kind) {
  case HCL2_NULL:
  case HCL2_UNKNOWN: /* not reached (unknown handled by caller); emit as null */
    return sb_puts(s, "null");
  case HCL2_BOOL:
    return sb_puts(s, v->b ? "true" : "false");
  case HCL2_NUMBER:
    snprintf(buf, sizeof(buf), "%g", v->num);
    return sb_puts(s, buf);
  case HCL2_STRING: {
    if (!sb_put(s, "\"", 1))
      return false;
    for (const char *p = v->str; *p; p++) {
      unsigned char c = (unsigned char)*p;
      switch (c) {
      case '"':
        sb_puts(s, "\\\"");
        break;
      case '\\':
        sb_puts(s, "\\\\");
        break;
      case '\n':
        sb_puts(s, "\\n");
        break;
      case '\r':
        sb_puts(s, "\\r");
        break;
      case '\t':
        sb_puts(s, "\\t");
        break;
      case '\b':
        sb_puts(s, "\\b");
        break;
      case '\f':
        sb_puts(s, "\\f");
        break;
      default:
        if (c < 0x20) {
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          sb_puts(s, buf);
        } else {
          sb_put(s, (const char *)&c, 1);
        }
      }
    }
    return sb_put(s, "\"", 1);
  }
  case HCL2_TUPLE:
  case HCL2_LIST:
  case HCL2_SET:
    if (!sb_put(s, "[", 1))
      return false;
    for (size_t i = 0; i < v->n; i++) {
      if (i > 0 && !sb_put(s, ",", 1))
        return false;
      if (!json_emit(v->items[i], s))
        return false;
    }
    return sb_put(s, "]", 1);
  default: /* HCL2_OBJECT / HCL2_MAP */
    if (!sb_put(s, "{", 1))
      return false;
    for (size_t i = 0; i < v->nf; i++) {
      if (i > 0 && !sb_put(s, ",", 1))
        return false;
      hcl2_value k = {0};
      k.kind = HCL2_STRING;
      k.str = v->fields[i].key;
      if (!json_emit(&k, s) || !sb_put(s, ":", 1) || !json_emit(v->fields[i].val, s))
        return false;
    }
    return sb_put(s, "}", 1);
  }
}

/* ---- collection functions (Terraform) ---- */
bool push_clone(hcl2_value *out, const hcl2_value *src) {
  hcl2_value *c = vclone(src);
  if (c == NULL)
    return false;
  if (!hcl2_tuple_push(out, c)) {
    hcl2_value_free(c);
    return false;
  }
  return true;
}

int sort_cmp(const void *x, const void *y) {
  const hcl2_value *const *px = x;
  const hcl2_value *const *py = y;
  return strcmp((*px)->str, (*py)->str);
}

bool flatten_into(hcl2_value *out, const hcl2_value *v) {
  if (hcl2_is_seq(v->kind)) {
    size_t L = hcl2_value_len(v);
    for (size_t i = 0; i < L; i++)
      if (!flatten_into(out, hcl2_value_at(v, i)))
        return false;
    return true;
  }
  return push_clone(out, v);
}

hcl2_value *boollist(const hcl2_value *const *a, size_t n, char *e, size_t es, bool all,
                     const char *who) {
  if (n != 1 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, who);
    return NULL;
  }
  size_t L = hcl2_value_len(a[0]);
  for (size_t i = 0; i < L; i++) {
    const hcl2_value *el = hcl2_value_at(a[0], i);
    if (el->kind != HCL2_BOOL) {
      everr(e, es, who);
      return NULL;
    }
    if (all && !el->b)
      return hcl2_bool(false);
    if (!all && el->b)
      return hcl2_bool(true);
  }
  return hcl2_bool(all);
}

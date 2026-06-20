#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* The printf-style format function, mirroring go-cty/Terraform's `format`.
 * Supported verbs: %% %v %s %q %t %d %o %x %X %b %e %E %f %F %g %G, with the
 * flags [-+ 0#], width, precision, and explicit argument indices (%[n]v). */

/* default (%v) rendering of a scalar; collections fall back to JSON. */
static bool fmt_default(struct sb *out, const hcl2_value *v) {
  char buf[64];
  switch (v->kind) {
  case HCL2_STRING:
    return sb_puts(out, v->str);
  case HCL2_BOOL:
    return sb_puts(out, v->b ? "true" : "false");
  case HCL2_NULL:
    return sb_puts(out, "null");
  case HCL2_NUMBER:
    if (v->num == (long long)v->num && fabs(v->num) < 1e15)
      snprintf(buf, sizeof buf, "%lld", (long long)v->num);
    else
      snprintf(buf, sizeof buf, "%g", v->num);
    return sb_puts(out, buf);
  default:
    return json_emit(v, out);
  }
}

/* run snprintf(spec, ...) into a right-sized buffer appended to out. */
static bool apply_str(struct sb *out, const char *spec, const char *s) {
  int need = snprintf(NULL, 0, spec, s);
  if (need < 0 || need > (1 << 20))
    return false;
  char *buf = malloc((size_t)need + 1);
  if (buf == NULL)
    return false;
  snprintf(buf, (size_t)need + 1, spec, s);
  bool ok = sb_put(out, buf, (size_t)need);
  free(buf);
  return ok;
}
static bool apply_ll(struct sb *out, const char *spec, long long v) {
  int need = snprintf(NULL, 0, spec, v);
  if (need < 0 || need > (1 << 20))
    return false;
  char *buf = malloc((size_t)need + 1);
  if (buf == NULL)
    return false;
  snprintf(buf, (size_t)need + 1, spec, v);
  bool ok = sb_put(out, buf, (size_t)need);
  free(buf);
  return ok;
}
static bool apply_dbl(struct sb *out, const char *spec, double v) {
  int need = snprintf(NULL, 0, spec, v);
  if (need < 0 || need > (1 << 20))
    return false;
  char *buf = malloc((size_t)need + 1);
  if (buf == NULL)
    return false;
  snprintf(buf, (size_t)need + 1, spec, v);
  bool ok = sb_put(out, buf, (size_t)need);
  free(buf);
  return ok;
}

/* quote a string Go-style ("..."), into a freshly malloc'd C string. */
static char *quote_str(const char *s) {
  struct sb q = {0};
  sb_put(&q, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
    case '"':
      sb_put(&q, "\\\"", 2);
      break;
    case '\\':
      sb_put(&q, "\\\\", 2);
      break;
    case '\n':
      sb_put(&q, "\\n", 2);
      break;
    case '\t':
      sb_put(&q, "\\t", 2);
      break;
    case '\r':
      sb_put(&q, "\\r", 2);
      break;
    default:
      if (*p < 0x20) {
        char b[8];
        snprintf(b, sizeof b, "\\x%02x", *p);
        sb_puts(&q, b);
      } else {
        sb_put(&q, (const char *)p, 1);
      }
    }
  }
  sb_put(&q, "\"", 1);
  if (q.oom) {
    free(q.p);
    return NULL;
  }
  return q.p ? q.p : strdup("\"\"");
}

hcl2_value *bi_format(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n < 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "format() needs a format string");
    return NULL;
  }
  const char *f = a[0]->str;
  size_t argi = 1;
  struct sb out = {0};
  for (const char *p = f; *p;) {
    if (*p != '%') {
      sb_put(&out, p, 1);
      p++;
      continue;
    }
    p++;
    if (*p == '%') {
      sb_put(&out, "%", 1);
      p++;
      continue;
    }
    long explicit_idx = -1;
    if (*p == '[') {
      p++;
      long k = 0;
      bool any = false;
      while (*p >= '0' && *p <= '9') {
        k = k * 10 + (*p - '0');
        p++;
        any = true;
      }
      if (*p != ']' || !any) {
        everr(e, es, "format() invalid argument index");
        free(out.p);
        return NULL;
      }
      p++;
      explicit_idx = k;
    }
    char spec[80];
    int si = 0;
    spec[si++] = '%';
    while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#')
      if (si < 70)
        spec[si++] = *p++;
      else
        p++;
    while (*p >= '0' && *p <= '9')
      if (si < 70)
        spec[si++] = *p++;
      else
        p++;
    if (*p == '.') {
      if (si < 70)
        spec[si++] = *p;
      p++;
      while (*p >= '0' && *p <= '9')
        if (si < 70)
          spec[si++] = *p++;
        else
          p++;
    }
    char verb = *p;
    if (verb)
      p++;
    size_t use = (explicit_idx >= 0) ? (size_t)explicit_idx : argi++;
    if (use < 1 || use >= n) {
      everr(e, es, "format() not enough arguments for verb");
      free(out.p);
      return NULL;
    }
    const hcl2_value *v = a[use];
    bool ok = true;
    if (verb == 's' || verb == 'v') {
      spec[si++] = 's';
      spec[si] = '\0';
      if (verb == 'v' && si == 3) { /* plain %v: render directly */
        ok = fmt_default(&out, v);
      } else if (v->kind == HCL2_STRING) {
        ok = apply_str(&out, spec, v->str);
      } else {
        struct sb tmp = {0};
        ok = fmt_default(&tmp, v) && !tmp.oom && apply_str(&out, spec, tmp.p ? tmp.p : "");
        free(tmp.p);
      }
    } else if (verb == 'q') {
      char *q = quote_str(v->kind == HCL2_STRING ? v->str : "");
      if (v->kind != HCL2_STRING) {
        free(q);
        everr(e, es, "format(): %q requires a string");
        free(out.p);
        return NULL;
      }
      spec[si++] = 's';
      spec[si] = '\0';
      ok = q != NULL && apply_str(&out, spec, q);
      free(q);
    } else if (verb == 't') {
      if (v->kind != HCL2_BOOL) {
        everr(e, es, "format(): %t requires a bool");
        free(out.p);
        return NULL;
      }
      spec[si++] = 's';
      spec[si] = '\0';
      ok = apply_str(&out, spec, v->b ? "true" : "false");
    } else if (verb == 'd' || verb == 'o' || verb == 'x' || verb == 'X') {
      if (v->kind != HCL2_NUMBER || v->num != floor(v->num)) {
        everr(e, es, "format(): integer verb requires a whole number");
        free(out.p);
        return NULL;
      }
      spec[si++] = 'l';
      spec[si++] = 'l';
      spec[si++] = verb;
      spec[si] = '\0';
      ok = apply_ll(&out, spec, (long long)v->num);
    } else if (verb == 'b') {
      if (v->kind != HCL2_NUMBER || v->num != floor(v->num)) {
        everr(e, es, "format(): %b requires a whole number");
        free(out.p);
        return NULL;
      }
      long long x = (long long)v->num;
      char bits[72];
      int bi = 0;
      unsigned long long u = x < 0 ? (unsigned long long)(-x) : (unsigned long long)x;
      if (u == 0)
        bits[bi++] = '0';
      while (u) {
        bits[bi++] = (char)('0' + (u & 1));
        u >>= 1;
      }
      struct sb t = {0};
      if (x < 0)
        sb_put(&t, "-", 1);
      for (int k = bi; k-- > 0;)
        sb_put(&t, &bits[k], 1);
      spec[si++] = 's';
      spec[si] = '\0';
      ok = !t.oom && apply_str(&out, spec, t.p ? t.p : "0");
      free(t.p);
    } else if (verb == 'e' || verb == 'E' || verb == 'f' || verb == 'F' || verb == 'g' ||
               verb == 'G') {
      if (v->kind != HCL2_NUMBER) {
        everr(e, es, "format(): float verb requires a number");
        free(out.p);
        return NULL;
      }
      spec[si++] = verb;
      spec[si] = '\0';
      ok = apply_dbl(&out, spec, v->num);
    } else {
      everr(e, es, "format(): unsupported verb");
      free(out.p);
      return NULL;
    }
    if (!ok) {
      free(out.p);
      return NULL;
    }
  }
  if (out.oom) {
    free(out.p);
    return NULL;
  }
  hcl2_value *r = hcl2_string(out.p ? out.p : "");
  free(out.p);
  return r;
}

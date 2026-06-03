#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * Builtin functions
 *
 * Each takes already-evaluated arguments and returns a fresh owned value, or
 * NULL on error (message via everr). Unknown propagation is handled by the
 * caller (an unknown argument makes the whole call unknown), so builtins never
 * see HCL2_UNKNOWN arguments.
 * ===========================================================================*/

/* ---- a tiny growable string buffer, for join() and jsonencode() ---- */
struct sb {
  char *p;
  size_t len, cap;
  bool oom;
};
static bool sb_put(struct sb *s, const char *d, size_t n) {
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
static bool sb_puts(struct sb *s, const char *str) { return sb_put(s, str, strlen(str)); }

/* ---- string ---- */
static hcl2_value *bi_length(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1) {
    everr(e, es, "length() takes 1 argument");
    return NULL;
  }
  const hcl2_value *v = a[0];
  if (v->kind == HCL2_STRING)
    return hcl2_number((double)strlen(v->str));
  if (hcl2_is_seq(v->kind) || hcl2_is_keyed(v->kind))
    return hcl2_number((double)hcl2_value_len(v));
  everr(e, es, "length() needs a string, tuple or object");
  return NULL;
}
static hcl2_value *str1(const hcl2_value *const *a, size_t n, char *e, size_t es, bool up) {
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
static hcl2_value *bi_upper(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return str1(a, n, e, es, true);
}
static hcl2_value *bi_lower(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return str1(a, n, e, es, false);
}
static hcl2_value *bi_join(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || !hcl2_is_seq(a[1]->kind)) {
    everr(e, es, "join() needs (string, tuple)");
    return NULL;
  }
  struct sb s = {0};
  for (size_t i = 0; i < a[1]->n; i++) {
    if (a[1]->items[i]->kind != HCL2_STRING) {
      everr(e, es, "join() tuple elements must be strings");
      free(s.p);
      return NULL;
    }
    if (i > 0)
      sb_puts(&s, a[0]->str);
    sb_puts(&s, a[1]->items[i]->str);
  }
  if (s.oom) {
    free(s.p);
    return NULL;
  }
  hcl2_value *v = hcl2_string(s.p ? s.p : "");
  free(s.p);
  return v;
}
static hcl2_value *bi_split(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "split() needs (string, string)");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  const char *sep = a[0]->str, *str = a[1]->str;
  if (*sep == '\0') { /* empty separator -> the whole string as one element */
    hcl2_value *s = hcl2_string(str);
    if (s == NULL || !hcl2_tuple_push(out, s)) {
      hcl2_value_free(s);
      hcl2_value_free(out);
      return NULL;
    }
    return out;
  }
  size_t seplen = strlen(sep);
  const char *p = str;
  for (;;) {
    const char *hit = strstr(p, sep);
    size_t seglen = hit ? (size_t)(hit - p) : strlen(p);
    char *seg = malloc(seglen + 1);
    if (seg == NULL) {
      hcl2_value_free(out);
      return NULL;
    }
    memcpy(seg, p, seglen);
    seg[seglen] = '\0';
    hcl2_value *sv = hcl2_string(seg);
    free(seg);
    if (sv == NULL || !hcl2_tuple_push(out, sv)) {
      hcl2_value_free(sv);
      hcl2_value_free(out);
      return NULL;
    }
    if (hit == NULL)
      return out;
    p = hit + seplen;
  }
}

/* ---- numeric ---- */
static hcl2_value *minmax(const hcl2_value *const *a, size_t n, char *e, size_t es, bool mx) {
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
static hcl2_value *bi_min(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return minmax(a, n, e, es, false);
}
static hcl2_value *bi_max(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return minmax(a, n, e, es, true);
}
static hcl2_value *num1(const hcl2_value *const *a, size_t n, char *e, size_t es,
                        double (*f)(double), const char *who) {
  if (n != 1 || a[0]->kind != HCL2_NUMBER) {
    everr(e, es, who);
    return NULL;
  }
  return hcl2_number(f(a[0]->num));
}
static hcl2_value *bi_abs(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return num1(a, n, e, es, fabs, "abs() needs one number");
}
static hcl2_value *bi_floor(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return num1(a, n, e, es, floor, "floor() needs one number");
}
static hcl2_value *bi_ceil(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return num1(a, n, e, es, ceil, "ceil() needs one number");
}

/* ---- collection ---- */
static hcl2_value *bi_concat(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    if (!hcl2_is_seq(a[i]->kind)) {
      everr(e, es, "concat() needs tuples");
      hcl2_value_free(out);
      return NULL;
    }
    for (size_t j = 0; j < a[i]->n; j++) {
      hcl2_value *c = vclone(a[i]->items[j]);
      if (c == NULL || !hcl2_tuple_push(out, c)) {
        hcl2_value_free(c);
        hcl2_value_free(out);
        return NULL;
      }
    }
  }
  return out;
}
static hcl2_value *bi_keys(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_keyed(a[0]->kind)) {
    everr(e, es, "keys() needs an object");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < a[0]->nf; i++) {
    hcl2_value *k = hcl2_string(a[0]->fields[i].key);
    if (k == NULL || !hcl2_tuple_push(out, k)) {
      hcl2_value_free(k);
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}
static hcl2_value *bi_values(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || !hcl2_is_keyed(a[0]->kind)) {
    everr(e, es, "values() needs an object");
    return NULL;
  }
  hcl2_value *out = hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (size_t i = 0; i < a[0]->nf; i++) {
    hcl2_value *v = vclone(a[0]->fields[i].val);
    if (v == NULL || !hcl2_tuple_push(out, v)) {
      hcl2_value_free(v);
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}
static hcl2_value *bi_contains(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || !hcl2_is_seq(a[0]->kind)) {
    everr(e, es, "contains() needs (tuple, value)");
    return NULL;
  }
  for (size_t i = 0; i < a[0]->n; i++)
    if (vequal(a[0]->items[i], a[1]))
      return hcl2_bool(true);
  return hcl2_bool(false);
}
static hcl2_value *bi_lookup(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 3 || !hcl2_is_keyed(a[0]->kind) || a[1]->kind != HCL2_STRING) {
    everr(e, es, "lookup() needs (object, string, default)");
    return NULL;
  }
  const hcl2_value *f = hcl2_value_get(a[0], a[1]->str);
  return vclone(f ? f : a[2]);
}
static hcl2_value *bi_coalesce(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  for (size_t i = 0; i < n; i++)
    if (a[i]->kind != HCL2_NULL)
      return vclone(a[i]);
  everr(e, es, "coalesce() received no non-null argument");
  return NULL;
}

/* ---- type conversion (wrap hcl2_convert) ---- */
static hcl2_value *conv1(const hcl2_value *const *a, size_t n, char *e, size_t es, hcl2_type *t,
                         const char *who) {
  if (n != 1) {
    everr(e, es, who);
    return NULL;
  }
  return hcl2_convert(a[0], t, e, es); /* t is a singleton: no free needed */
}
static hcl2_value *bi_tostring(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return conv1(a, n, e, es, hcl2_type_string(), "tostring() takes 1 argument");
}
static hcl2_value *bi_tonumber(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return conv1(a, n, e, es, hcl2_type_number(), "tonumber() takes 1 argument");
}
static hcl2_value *bi_tobool(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  return conv1(a, n, e, es, hcl2_type_bool(), "tobool() takes 1 argument");
}

/* ---- JSON ---- */
static hcl2_value *bi_jsondecode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "jsondecode() needs a string");
    return NULL;
  }
  return hcl2_parse_json(a[0]->str, strlen(a[0]->str), e, es);
}
static bool json_emit(const hcl2_value *v, struct sb *s) {
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
static hcl2_value *bi_jsonencode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1) {
    everr(e, es, "jsonencode() takes 1 argument");
    return NULL;
  }
  struct sb s = {0};
  if (!json_emit(a[0], &s)) { /* json_emit only fails on OOM (-> NULL, like elsewhere) */
    free(s.p);
    return NULL;
  }
  hcl2_value *v = hcl2_string(s.p ? s.p : "");
  free(s.p);
  return v;
}

hcl2_func builtin_func(const char *name) {
  static const struct {
    const char *name;
    hcl2_func fn;
  } table[] = {
      {"length",     bi_length    },
      {"upper",      bi_upper     },
      {"lower",      bi_lower     },
      {"min",        bi_min       },
      {"max",        bi_max       },
      {"join",       bi_join      },
      {"split",      bi_split     },
      {"abs",        bi_abs       },
      {"floor",      bi_floor     },
      {"ceil",       bi_ceil      },
      {"concat",     bi_concat    },
      {"keys",       bi_keys      },
      {"values",     bi_values    },
      {"contains",   bi_contains  },
      {"lookup",     bi_lookup    },
      {"coalesce",   bi_coalesce  },
      {"tostring",   bi_tostring  },
      {"tonumber",   bi_tonumber  },
      {"tobool",     bi_tobool    },
      {"jsondecode", bi_jsondecode},
      {"jsonencode", bi_jsonencode},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    if (strcmp(table[i].name, name) == 0)
      return table[i].fn;
  return NULL;
}

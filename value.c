#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#ifdef HCL2_FAULT_INJECT
int hcl2_alloc_budget = -1; /* storage; wrappers + macros live in hcl2_alloc.h */
#endif

/* ===========================================================================
 * Value model
 * ===========================================================================*/
/* struct kv / struct hcl2_value live in hcl2_internal.h (shared with hcl2.c). */

static hcl2_value *vnew(hcl2_kind k) {
  hcl2_value *v = calloc(1, sizeof(*v));
  if (v != NULL)
    v->kind = k;
  return v;
}
hcl2_value *hcl2_null(void) { return vnew(HCL2_NULL); }
hcl2_value *hcl2_unknown(void) { return vnew(HCL2_UNKNOWN); }
hcl2_value *hcl2_bool(bool b) {
  hcl2_value *v = vnew(HCL2_BOOL);
  if (v)
    v->b = b;
  return v;
}
hcl2_value *hcl2_number(double n) {
  hcl2_value *v = vnew(HCL2_NUMBER);
  if (v)
    v->num = n;
  return v;
}
hcl2_value *hcl2_string(const char *s) {
  hcl2_value *v = vnew(HCL2_STRING);
  if (v == NULL)
    return NULL;
  v->str = strdup(s ? s : "");
  if (v->str == NULL) {
    free(v);
    return NULL;
  }
  return v;
}
hcl2_value *hcl2_tuple(void) { return vnew(HCL2_TUPLE); }
hcl2_value *hcl2_object(void) { return vnew(HCL2_OBJECT); }

void hcl2_value_free(hcl2_value *v) {
  if (v == NULL)
    return;
  free(v->str);
  for (size_t i = 0; i < v->n; i++)
    hcl2_value_free(v->items[i]);
  free(v->items);
  for (size_t i = 0; i < v->nf; i++) {
    free(v->fields[i].key);
    hcl2_value_free(v->fields[i].val);
  }
  free(v->fields);
  free(v);
}

bool hcl2_tuple_push(hcl2_value *t, hcl2_value *e) {
  if (t == NULL || t->kind != HCL2_TUPLE || e == NULL)
    return false;
  hcl2_value **ni = realloc(t->items, (t->n + 1) * sizeof(*ni));
  if (ni == NULL)
    return false;
  t->items = ni;
  t->items[t->n++] = e;
  return true;
}
bool hcl2_object_set(hcl2_value *o, const char *key, hcl2_value *val) {
  if (o == NULL || o->kind != HCL2_OBJECT || val == NULL)
    return false;
  for (size_t i = 0; i < o->nf; i++) {
    if (strcmp(o->fields[i].key, key) == 0) {
      hcl2_value_free(o->fields[i].val);
      o->fields[i].val = val;
      return true;
    }
  }
  struct kv *nf = realloc(o->fields, (o->nf + 1) * sizeof(*nf));
  if (nf == NULL)
    return false;
  o->fields = nf;
  o->fields[o->nf].key = strdup(key);
  if (o->fields[o->nf].key == NULL)
    return false;
  o->fields[o->nf].val = val;
  o->nf++;
  return true;
}

hcl2_kind hcl2_value_kind(const hcl2_value *v) { return v->kind; }
bool hcl2_value_is_unknown(const hcl2_value *v) { return v != NULL && v->kind == HCL2_UNKNOWN; }
bool hcl2_value_as_bool(const hcl2_value *v, bool *out) {
  if (v == NULL || v->kind != HCL2_BOOL)
    return false;
  if (out)
    *out = v->b;
  return true;
}
bool hcl2_value_as_number(const hcl2_value *v, double *out) {
  if (v == NULL || v->kind != HCL2_NUMBER)
    return false;
  if (out)
    *out = v->num;
  return true;
}
const char *hcl2_value_as_string(const hcl2_value *v) {
  return (v != NULL && v->kind == HCL2_STRING) ? v->str : NULL;
}
size_t hcl2_value_len(const hcl2_value *v) {
  if (v == NULL)
    return 0;
  if (v->kind == HCL2_TUPLE)
    return v->n;
  if (v->kind == HCL2_OBJECT)
    return v->nf;
  return 0;
}
const hcl2_value *hcl2_value_at(const hcl2_value *v, size_t i) {
  return (v != NULL && v->kind == HCL2_TUPLE && i < v->n) ? v->items[i] : NULL;
}
const hcl2_value *hcl2_value_get(const hcl2_value *v, const char *key) {
  if (v == NULL || v->kind != HCL2_OBJECT)
    return NULL;
  for (size_t i = 0; i < v->nf; i++)
    if (strcmp(v->fields[i].key, key) == 0)
      return v->fields[i].val;
  return NULL;
}

hcl2_value *vclone(const hcl2_value *v) {
  if (v == NULL)
    return NULL;
  switch (v->kind) {
  case HCL2_NULL:
    return hcl2_null();
  case HCL2_UNKNOWN:
    return hcl2_unknown();
  case HCL2_BOOL:
    return hcl2_bool(v->b);
  case HCL2_NUMBER:
    return hcl2_number(v->num);
  case HCL2_STRING:
    return hcl2_string(v->str);
  case HCL2_TUPLE: {
    hcl2_value *t = hcl2_tuple();
    for (size_t i = 0; i < v->n; i++) {
      hcl2_value *e = vclone(v->items[i]);
      if (e == NULL || !hcl2_tuple_push(t, e)) {
        hcl2_value_free(e);
        hcl2_value_free(t);
        return NULL;
      }
    }
    return t;
  }
  case HCL2_OBJECT: {
    hcl2_value *o = hcl2_object();
    for (size_t i = 0; i < v->nf; i++) {
      hcl2_value *e = vclone(v->fields[i].val);
      if (e == NULL || !hcl2_object_set(o, v->fields[i].key, e)) {
        hcl2_value_free(e);
        hcl2_value_free(o);
        return NULL;
      }
    }
    return o;
  }
  }
  return NULL;
}

bool vequal(const hcl2_value *a, const hcl2_value *b) {
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case HCL2_NULL:
    return true;
  case HCL2_UNKNOWN:
    return true; /* two unknowns compare equal (e.g. for set de-duplication) */
  case HCL2_BOOL:
    return a->b == b->b;
  case HCL2_NUMBER:
    return a->num == b->num;
  case HCL2_STRING:
    return strcmp(a->str, b->str) == 0;
  case HCL2_TUPLE:
    if (a->n != b->n)
      return false;
    for (size_t i = 0; i < a->n; i++)
      if (!vequal(a->items[i], b->items[i]))
        return false;
    return true;
  case HCL2_OBJECT:
    if (a->nf != b->nf)
      return false;
    for (size_t i = 0; i < a->nf; i++) {
      const hcl2_value *bv = hcl2_value_get(b, a->fields[i].key);
      if (bv == NULL || !vequal(a->fields[i].val, bv))
        return false;
    }
    return true;
  }
  return false;
}

/* ===========================================================================
 * Context
 * ===========================================================================*/
struct var {
  char *name;
  hcl2_value *val;
};
struct fn {
  char *name;
  hcl2_func fn;
};
struct hcl2_ctx {
  struct var *vars;
  size_t nv;
  struct fn *fns;
  size_t nf;
};

hcl2_ctx *hcl2_ctx_new(void) { return calloc(1, sizeof(struct hcl2_ctx)); }
void hcl2_ctx_free(hcl2_ctx *c) {
  if (c == NULL)
    return;
  for (size_t i = 0; i < c->nv; i++) {
    free(c->vars[i].name);
    hcl2_value_free(c->vars[i].val);
  }
  free(c->vars);
  for (size_t i = 0; i < c->nf; i++)
    free(c->fns[i].name);
  free(c->fns);
  free(c);
}
bool hcl2_ctx_set_var(hcl2_ctx *c, const char *name, hcl2_value *v) {
  for (size_t i = 0; i < c->nv; i++) {
    if (strcmp(c->vars[i].name, name) == 0) {
      hcl2_value_free(c->vars[i].val);
      c->vars[i].val = v;
      return true;
    }
  }
  struct var *nv = realloc(c->vars, (c->nv + 1) * sizeof(*nv));
  if (nv == NULL)
    return false;
  c->vars = nv;
  c->vars[c->nv].name = strdup(name);
  if (c->vars[c->nv].name == NULL)
    return false;
  c->vars[c->nv].val = v;
  c->nv++;
  return true;
}
bool hcl2_ctx_set_func(hcl2_ctx *c, const char *name, hcl2_func fn) {
  struct fn *nf = realloc(c->fns, (c->nf + 1) * sizeof(*nf));
  if (nf == NULL)
    return false;
  c->fns = nf;
  c->fns[c->nf].name = strdup(name);
  if (c->fns[c->nf].name == NULL)
    return false;
  c->fns[c->nf].fn = fn;
  c->nf++;
  return true;
}
const hcl2_value *ctx_var(hcl2_ctx *c, const char *name) {
  if (c == NULL)
    return NULL;
  for (size_t i = 0; i < c->nv; i++)
    if (strcmp(c->vars[i].name, name) == 0)
      return c->vars[i].val;
  return NULL;
}
hcl2_value *ctx_take_var(hcl2_ctx *c, const char *name) {
  if (c == NULL)
    return NULL;
  for (size_t i = 0; i < c->nv; i++) {
    if (strcmp(c->vars[i].name, name) == 0) {
      hcl2_value *v = c->vars[i].val;
      free(c->vars[i].name);
      memmove(&c->vars[i], &c->vars[i + 1], (c->nv - i - 1) * sizeof(c->vars[0]));
      c->nv--;
      return v;
    }
  }
  return NULL;
}
hcl2_func ctx_func(hcl2_ctx *c, const char *name) {
  if (c == NULL)
    return NULL;
  for (size_t i = 0; i < c->nf; i++)
    if (strcmp(c->fns[i].name, name) == 0)
      return c->fns[i].fn;
  return NULL;
}

/* ===========================================================================
 * Builtin functions
 * ===========================================================================*/
static hcl2_value *bi_length(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1) {
    everr(e, es, "length() takes 1 argument");
    return NULL;
  }
  const hcl2_value *v = a[0];
  if (v->kind == HCL2_STRING)
    return hcl2_number((double)strlen(v->str));
  if (v->kind == HCL2_TUPLE || v->kind == HCL2_OBJECT)
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

hcl2_func builtin_func(const char *name) {
  static const struct {
    const char *name;
    hcl2_func fn;
  } table[] = {
      {"length", bi_length},
      {"upper",  bi_upper },
      {"lower",  bi_lower },
      {"min",    bi_min   },
      {"max",    bi_max   },
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    if (strcmp(table[i].name, name) == 0)
      return table[i].fn;
  return NULL;
}

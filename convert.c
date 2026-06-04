#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * Type constraints & conversion (M4)
 *
 * Types are used as constraints. Because the value model has no distinct
 * list/set/map runtime kind yet, list/set convert to a (homogeneous) tuple and
 * map to an object; the conversion validates the shape and coerces every
 * element/value to the element type. Primitive coercions follow cty:
 *   - to string: number ("%g") and bool ("true"/"false") convert; string is id.
 *   - to number: a fully-numeric string converts; number is id.
 *   - to bool:   "true"/"false" strings convert; bool is id.
 *   - to any:    identity.
 * ===========================================================================*/

enum type_kind { TY_ANY, TY_BOOL, TY_NUMBER, TY_STRING, TY_LIST, TY_SET, TY_MAP };

struct hcl2_type {
  enum type_kind kind;
  hcl2_type *elem; /* LIST/SET/MAP element type (owned unless is_static) */
  bool is_static;  /* a primitive singleton: hcl2_type_free leaves it alone */
};

static hcl2_type T_ANY = {TY_ANY, NULL, true};
static hcl2_type T_BOOL = {TY_BOOL, NULL, true};
static hcl2_type T_NUMBER = {TY_NUMBER, NULL, true};
static hcl2_type T_STRING = {TY_STRING, NULL, true};

hcl2_type *hcl2_type_any(void) { return &T_ANY; }
hcl2_type *hcl2_type_bool(void) { return &T_BOOL; }
hcl2_type *hcl2_type_number(void) { return &T_NUMBER; }
hcl2_type *hcl2_type_string(void) { return &T_STRING; }

void hcl2_type_free(hcl2_type *t) {
  if (t == NULL || t->is_static)
    return;
  hcl2_type_free(t->elem);
  free(t);
}

static hcl2_type *coll(enum type_kind k, hcl2_type *elem) {
  hcl2_type *t = calloc(1, sizeof(*t));
  if (t == NULL) {
    hcl2_type_free(elem); /* consume ownership even on failure */
    return NULL;
  }
  t->kind = k;
  t->elem = elem;
  return t;
}
hcl2_type *hcl2_type_list(hcl2_type *elem) { return coll(TY_LIST, elem); }
hcl2_type *hcl2_type_set(hcl2_type *elem) { return coll(TY_SET, elem); }
hcl2_type *hcl2_type_map(hcl2_type *elem) { return coll(TY_MAP, elem); }

/* Deep-copy a type constraint. Primitive singletons (and NULL) are shared, not
 * copied -- hcl2_type_free leaves them alone. Collection types are rebuilt.
 * Returns NULL only when cloning a collection runs out of memory. */
hcl2_type *type_clone(const hcl2_type *t) {
  if (t == NULL || t->is_static)
    return (hcl2_type *)t;
  hcl2_type *e = type_clone(t->elem);
  if (t->elem != NULL && e == NULL)
    return NULL; /* OOM cloning the element */
  return coll(t->kind, e);
}

static hcl2_value *num_to_string(double n) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%g", n);
  return hcl2_string(buf);
}

hcl2_value *hcl2_convert(const hcl2_value *v, const hcl2_type *t, char *err, size_t errsz) {
  if (err != NULL && errsz > 0)
    err[0] = '\0';
  if (v == NULL || t == NULL) {
    everr(err, errsz, "convert: null argument");
    return NULL;
  }
  if (v->kind == HCL2_UNKNOWN) {
    /* An unknown stays unknown but is refined to the target type. Converting to
     * `any` is the identity, so it keeps whatever type it already carried. */
    if (t->kind == TY_ANY)
      return vclone(v);
    hcl2_type *ct = type_clone(t);
    if (ct == NULL) { /* OOM cloning a collection target type */
      everr(err, errsz, "convert: out of memory");
      return NULL;
    }
    return hcl2_unknown_of(ct);
  }
  switch (t->kind) {
  case TY_ANY:
    return vclone(v);
  case TY_BOOL:
    if (v->kind == HCL2_BOOL)
      return vclone(v);
    if (v->kind == HCL2_STRING && strcmp(v->str, "true") == 0)
      return hcl2_bool(true);
    if (v->kind == HCL2_STRING && strcmp(v->str, "false") == 0)
      return hcl2_bool(false);
    everr(err, errsz, "convert: value is not convertible to bool");
    return NULL;
  case TY_NUMBER:
    if (v->kind == HCL2_NUMBER)
      return vclone(v);
    if (v->kind == HCL2_STRING) {
      char *endp = NULL;
      double d = strtod(v->str, &endp);
      if (endp != v->str && *endp == '\0')
        return hcl2_number(d);
    }
    everr(err, errsz, "convert: value is not convertible to number");
    return NULL;
  case TY_STRING:
    if (v->kind == HCL2_STRING)
      return vclone(v);
    if (v->kind == HCL2_NUMBER)
      return num_to_string(v->num);
    if (v->kind == HCL2_BOOL)
      return hcl2_string(v->b ? "true" : "false");
    everr(err, errsz, "convert: value is not convertible to string");
    return NULL;
  case TY_LIST:
  case TY_SET: {
    if (!hcl2_is_seq(v->kind)) {
      everr(err, errsz, "convert: source of a list/set must be a tuple, list or set");
      return NULL;
    }
    hcl2_value *out = (t->kind == TY_SET) ? hcl2_set() : hcl2_list();
    if (out == NULL)
      return NULL;
    for (size_t i = 0; i < v->n; i++) {
      hcl2_value *e = hcl2_convert(v->items[i], t->elem, err, errsz);
      if (e == NULL) {
        hcl2_value_free(out);
        return NULL;
      }
      if (t->kind == TY_SET) {
        bool dup = false;
        for (size_t j = 0; j < out->n; j++)
          if (vequal(out->items[j], e)) {
            dup = true;
            break;
          }
        if (dup) {
          hcl2_value_free(e);
          continue;
        }
      }
      if (!hcl2_tuple_push(out, e)) {
        hcl2_value_free(e);
        hcl2_value_free(out);
        return NULL;
      }
    }
    return out;
  }
  default: { /* TY_MAP */
    if (!hcl2_is_keyed(v->kind)) {
      everr(err, errsz, "convert: source of a map must be an object or map");
      return NULL;
    }
    hcl2_value *out = hcl2_map();
    if (out == NULL)
      return NULL;
    for (size_t i = 0; i < v->nf; i++) {
      hcl2_value *e = hcl2_convert(v->fields[i].val, t->elem, err, errsz);
      if (e == NULL || !hcl2_object_set(out, v->fields[i].key, e)) {
        hcl2_value_free(e);
        hcl2_value_free(out);
        return NULL;
      }
    }
    return out;
  }
  }
}

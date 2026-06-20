#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* yamlencode: emit a value as a YAML document in block style, with object keys
 * sorted and every string scalar double-quoted -- matching Terraform's
 * deterministic output. Sequences nested under a key keep the key's
 * indentation; mappings nest two spaces deeper. */

struct ykv {
  const char *key;
  const hcl2_value *val;
};

static int kvcmp(const void *a, const void *b) {
  return strcmp(((const struct ykv *)a)->key, ((const struct ykv *)b)->key);
}

static bool is_scalar(const hcl2_value *v) {
  return v->kind == HCL2_NULL || v->kind == HCL2_UNKNOWN || v->kind == HCL2_BOOL ||
         v->kind == HCL2_NUMBER || v->kind == HCL2_STRING;
}

static bool empty_coll(const hcl2_value *v) {
  return (hcl2_is_seq(v->kind) && v->n == 0) || (hcl2_is_keyed(v->kind) && v->nf == 0);
}

static void emit_indent(struct sb *s, int n) {
  for (int i = 0; i < n; i++)
    sb_put(s, " ", 1);
}

static bool emit_qstring(struct sb *s, const char *str) {
  char buf[16];
  sb_put(s, "\"", 1);
  for (const char *p = str; *p; p++) {
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
    default:
      if (c < 0x20) {
        snprintf(buf, sizeof buf, "\\u%04x", c);
        sb_puts(s, buf);
      } else {
        sb_put(s, (const char *)&c, 1);
      }
    }
  }
  return sb_put(s, "\"", 1);
}

static bool emit_scalar(struct sb *s, const hcl2_value *v) {
  char buf[40];
  switch (v->kind) {
  case HCL2_NULL:
  case HCL2_UNKNOWN:
    return sb_puts(s, "null");
  case HCL2_BOOL:
    return sb_puts(s, v->b ? "true" : "false");
  case HCL2_NUMBER:
    snprintf(buf, sizeof buf, "%g", v->num);
    return sb_puts(s, buf);
  default: /* HCL2_STRING */
    return emit_qstring(s, v->str);
  }
}

/* emit an inline value for empty collections ([] / {}). */
static void emit_empty(struct sb *s, const hcl2_value *v) {
  sb_put(s, " ", 1);
  sb_puts(s, hcl2_is_seq(v->kind) ? "[]" : "{}");
  sb_put(s, "\n", 1);
}

static bool emit_block(struct sb *s, const hcl2_value *v, int indent) {
  if (hcl2_is_seq(v->kind)) {
    if (v->n == 0) {
      emit_indent(s, indent);
      sb_puts(s, "[]\n");
      return !s->oom;
    }
    for (size_t i = 0; i < v->n; i++) {
      const hcl2_value *it = v->items[i];
      emit_indent(s, indent);
      sb_put(s, "-", 1);
      if (is_scalar(it)) {
        sb_put(s, " ", 1);
        emit_scalar(s, it);
        sb_put(s, "\n", 1);
      } else if (empty_coll(it)) {
        emit_empty(s, it);
      } else {
        sb_put(s, "\n", 1);
        emit_block(s, it, indent + 2);
      }
    }
    return !s->oom;
  }
  if (hcl2_is_keyed(v->kind)) {
    if (v->nf == 0) {
      emit_indent(s, indent);
      sb_puts(s, "{}\n");
      return !s->oom;
    }
    struct ykv *kvs = malloc(v->nf * sizeof *kvs);
    if (kvs == NULL)
      return false;
    for (size_t i = 0; i < v->nf; i++) {
      kvs[i].key = v->fields[i].key;
      kvs[i].val = v->fields[i].val;
    }
    qsort(kvs, v->nf, sizeof *kvs, kvcmp);
    for (size_t i = 0; i < v->nf; i++) {
      const hcl2_value *val = kvs[i].val;
      emit_indent(s, indent);
      emit_qstring(s, kvs[i].key);
      sb_put(s, ":", 1);
      if (is_scalar(val)) {
        sb_put(s, " ", 1);
        emit_scalar(s, val);
        sb_put(s, "\n", 1);
      } else if (empty_coll(val)) {
        emit_empty(s, val);
      } else if (hcl2_is_seq(val->kind)) {
        sb_put(s, "\n", 1);
        emit_block(s, val, indent); /* sequence keeps the key's indent */
      } else {
        sb_put(s, "\n", 1);
        emit_block(s, val, indent + 2);
      }
    }
    free(kvs);
    return !s->oom;
  }
  emit_indent(s, indent);
  emit_scalar(s, v);
  sb_put(s, "\n", 1);
  return !s->oom;
}

hcl2_value *bi_yamlencode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1) {
    everr(e, es, "yamlencode() takes 1 argument");
    return NULL;
  }
  struct sb b = {0};
  if (!emit_block(&b, a[0], 0) || b.oom) {
    free(b.p);
    return NULL;
  }
  hcl2_value *r = hcl2_string(b.p != NULL ? b.p : "");
  free(b.p);
  return r;
}

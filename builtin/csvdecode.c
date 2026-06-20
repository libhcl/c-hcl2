#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* csvdecode: parse an RFC 4180-ish CSV document (first row = header) into a
 * tuple of objects {header: value}. All values are strings. */

static bool emit_field(hcl2_value *row, struct sb *field) {
  if (field->oom)
    return false;
  hcl2_value *s = hcl2_string(field->p ? field->p : "");
  field->len = 0;
  if (field->p)
    field->p[0] = '\0';
  if (s == NULL || !hcl2_tuple_push(row, s)) {
    hcl2_value_free(s);
    return false;
  }
  return true;
}

/* consumes `row`; first row becomes *header (ownership transferred). */
static bool emit_row(hcl2_value *out, hcl2_value **header, hcl2_value *row, char *e, size_t es) {
  if (*header == NULL) {
    *header = row;
    return true;
  }
  size_t hc = hcl2_value_len(*header), rc = hcl2_value_len(row);
  if (hc != rc) {
    everr(e, es, "csvdecode() row has a different number of fields than the header");
    hcl2_value_free(row);
    return false;
  }
  hcl2_value *obj = hcl2_object();
  if (obj == NULL) {
    hcl2_value_free(row);
    return false;
  }
  for (size_t i = 0; i < hc; i++) {
    hcl2_value *val = vclone(hcl2_value_at(row, i));
    if (val == NULL || !hcl2_object_set(obj, hcl2_value_at(*header, i)->str, val)) {
      hcl2_value_free(val);
      hcl2_value_free(obj);
      hcl2_value_free(row);
      return false;
    }
  }
  hcl2_value_free(row);
  if (!hcl2_tuple_push(out, obj)) {
    hcl2_value_free(obj);
    return false;
  }
  return true;
}

hcl2_value *bi_csvdecode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "csvdecode() needs one string");
    return NULL;
  }
  const char *p = a[0]->str;
  hcl2_value *out = hcl2_tuple();
  hcl2_value *header = NULL;
  hcl2_value *row = out ? hcl2_tuple() : NULL;
  struct sb field = {0};
  bool quoted = false, open = false;
  if (out == NULL || row == NULL)
    goto fail;

  while (*p) {
    char c = *p;
    if (quoted) {
      if (c == '"') {
        if (p[1] == '"') {
          sb_put(&field, "\"", 1);
          p += 2;
          continue;
        }
        quoted = false;
        p++;
        continue;
      }
      sb_put(&field, &c, 1);
      p++;
      continue;
    }
    if (c == '"') {
      quoted = true;
      open = true;
      p++;
      continue;
    }
    if (c == ',') {
      if (!emit_field(row, &field))
        goto fail;
      open = true;
      p++;
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (c == '\r' && p[1] == '\n')
        p++;
      if (open || field.len > 0 || hcl2_value_len(row) > 0) {
        if (!emit_field(row, &field))
          goto fail;
        if (!emit_row(out, &header, row, e, es)) {
          row = NULL;
          goto fail;
        }
        row = hcl2_tuple();
        if (row == NULL)
          goto fail;
      }
      open = false;
      p++;
      continue;
    }
    sb_put(&field, &c, 1);
    open = true;
    p++;
  }
  if (open || field.len > 0 || hcl2_value_len(row) > 0) {
    if (!emit_field(row, &field))
      goto fail;
    if (!emit_row(out, &header, row, e, es)) {
      row = NULL;
      goto fail;
    }
    row = NULL;
  } else {
    hcl2_value_free(row);
    row = NULL;
  }
  free(field.p);
  hcl2_value_free(header);
  return out;

fail:
  free(field.p);
  hcl2_value_free(row);
  hcl2_value_free(header);
  hcl2_value_free(out);
  return NULL;
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* A pragmatic YAML-subset decoder: block mappings/sequences (indentation
 * based), flow collections [..] {..}, and scalars with core-schema type
 * inference (null/~, true/false, int, float, quoted and plain strings),
 * literal/folded block scalars (| / >, with -/+ chomping, via a pre-pass),
 * node anchors/aliases (&a / *a), core-schema tags (!!str / !!int / ...),
 * merge keys (<<) and multi-document streams (--- / ..., decoded to a tuple of
 * documents -- a superset of Terraform, which accepts only one document). */

struct yline {
  int indent;
  char *text; /* comment-stripped, trailing-trimmed; NUL-terminated */
};

struct yanchor {
  char *name;
  hcl2_value *val;
};

struct yp {
  struct yline *lines;
  size_t n, pos;
  char *err;
  size_t errsz;
  bool failed;
  struct yanchor *anch; /* &name anchors, for *name alias resolution */
  size_t nanch, canch;
};

/* YAML core-schema tag coercions handled explicitly; others are accepted and
 * ignored (the value keeps its inferred type). */
enum { TAG_NONE, TAG_STR, TAG_INT, TAG_FLOAT, TAG_BOOL, TAG_NULL };

static int tag_from(const char *n, size_t len) {
  while (len && *n == '!') { /* skip the leading ! / !! */
    n++;
    len--;
  }
  if (len == 3 && memcmp(n, "str", 3) == 0)
    return TAG_STR;
  if (len == 3 && memcmp(n, "int", 3) == 0)
    return TAG_INT;
  if (len == 5 && memcmp(n, "float", 5) == 0)
    return TAG_FLOAT;
  if (len == 4 && memcmp(n, "bool", 4) == 0)
    return TAG_BOOL;
  if (len == 4 && memcmp(n, "null", 4) == 0)
    return TAG_NULL;
  return TAG_NONE;
}

/* consume any leading "&anchor" / "!tag" / "!!tag" node-property tokens. */
static void parse_prefix(const char **s, char **anchor, int *tag) {
  *anchor = NULL;
  *tag = TAG_NONE;
  for (;;) {
    while (**s == ' ')
      (*s)++;
    if (**s != '&' && **s != '!')
      break;
    char lead = **s;
    (*s)++;
    const char *n = *s;
    while (**s && **s != ' ' && **s != ',' && **s != ']' && **s != '}')
      (*s)++;
    size_t len = (size_t)(*s - n);
    if (lead == '&') {
      free(*anchor);
      *anchor = malloc(len + 1);
      if (*anchor != NULL) {
        memcpy(*anchor, n, len);
        (*anchor)[len] = '\0';
      }
    } else {
      *tag = tag_from(n, len);
    }
  }
  while (**s == ' ')
    (*s)++;
}

static void anchor_register(struct yp *p, const char *name, const hcl2_value *v) {
  if (name == NULL || v == NULL)
    return;
  hcl2_value *copy = vclone(v);
  if (copy == NULL)
    return;
  for (size_t i = 0; i < p->nanch; i++)
    if (strcmp(p->anch[i].name, name) == 0) { /* last definition wins */
      hcl2_value_free(p->anch[i].val);
      p->anch[i].val = copy;
      return;
    }
  if (p->nanch == p->canch) {
    size_t nc = p->canch ? p->canch * 2 : 4;
    struct yanchor *na = realloc(p->anch, nc * sizeof *na);
    if (na == NULL) {
      hcl2_value_free(copy);
      return;
    }
    p->anch = na;
    p->canch = nc;
  }
  p->anch[p->nanch].name = strdup(name);
  if (p->anch[p->nanch].name == NULL) {
    hcl2_value_free(copy);
    return;
  }
  p->anch[p->nanch].val = copy;
  p->nanch++;
}

static hcl2_value *anchor_lookup(struct yp *p, const char *name, size_t len) {
  for (size_t i = 0; i < p->nanch; i++)
    if (strlen(p->anch[i].name) == len && memcmp(p->anch[i].name, name, len) == 0)
      return vclone(p->anch[i].val);
  return NULL;
}

/* coerce a parsed value to an explicit core-schema tag (leniently). */
static hcl2_value *apply_tag(hcl2_value *v, int tag) {
  if (v == NULL || tag == TAG_NONE)
    return v;
  if (tag == TAG_NULL) {
    hcl2_value_free(v);
    return hcl2_null();
  }
  if (tag == TAG_STR) {
    if (v->kind == HCL2_STRING)
      return v;
    char buf[40];
    const char *s = buf;
    if (v->kind == HCL2_NUMBER)
      snprintf(buf, sizeof buf, "%g", v->num);
    else if (v->kind == HCL2_BOOL)
      s = v->b ? "true" : "false";
    else if (v->kind == HCL2_NULL)
      s = "null";
    else
      return v; /* collections: leave as-is */
    hcl2_value *r = hcl2_string(s);
    hcl2_value_free(v);
    return r;
  }
  if (tag == TAG_BOOL) {
    if (v->kind == HCL2_STRING && (strcmp(v->str, "true") == 0 || strcmp(v->str, "false") == 0)) {
      hcl2_value *r = hcl2_bool(v->str[0] == 't');
      hcl2_value_free(v);
      return r;
    }
    return v;
  }
  if ((tag == TAG_INT || tag == TAG_FLOAT) && v->kind == HCL2_STRING) {
    char *end;
    double d = strtod(v->str, &end);
    if (*end == '\0') {
      hcl2_value *r = hcl2_number(d);
      hcl2_value_free(v);
      return r;
    }
  }
  return v;
}

static hcl2_value *fail(struct yp *p, const char *m) {
  if (!p->failed) {
    everr(p->err, p->errsz, m);
    p->failed = true;
  }
  return NULL;
}

/* ---- scalar parsing ---------------------------------------------------- */

/* decode a quoted scalar starting at *s ('"' or '\''); advance *s past it. */
static char *parse_quoted(const char **s, struct yp *p) {
  char q = **s;
  const char *c = *s + 1;
  struct sb b = {0};
  if (q == '\'') {
    for (;;) {
      if (*c == '\0') {
        fail(p, "yamldecode: unterminated single-quoted string");
        free(b.p);
        return NULL;
      }
      if (*c == '\'') {
        if (c[1] == '\'') { /* '' -> literal ' */
          sb_put(&b, "'", 1);
          c += 2;
          continue;
        }
        c++;
        break;
      }
      sb_put(&b, c, 1);
      c++;
    }
  } else {
    for (;;) {
      if (*c == '\0') {
        fail(p, "yamldecode: unterminated double-quoted string");
        free(b.p);
        return NULL;
      }
      if (*c == '"') {
        c++;
        break;
      }
      if (*c == '\\') {
        c++;
        char e = *c;
        switch (e) {
        case 'n':
          sb_put(&b, "\n", 1);
          break;
        case 't':
          sb_put(&b, "\t", 1);
          break;
        case 'r':
          sb_put(&b, "\r", 1);
          break;
        case 'b':
          sb_put(&b, "\b", 1);
          break;
        case 'f':
          sb_put(&b, "\f", 1);
          break;
        case '0':
          sb_put(&b, "\0", 1);
          break;
        case '"':
          sb_put(&b, "\"", 1);
          break;
        case '\\':
          sb_put(&b, "\\", 1);
          break;
        case '/':
          sb_put(&b, "/", 1);
          break;
        default:
          sb_put(&b, &e, 1);
          break;
        }
        if (e == '\0') {
          fail(p, "yamldecode: bad escape");
          free(b.p);
          return NULL;
        }
        c++;
        continue;
      }
      sb_put(&b, c, 1);
      c++;
    }
  }
  *s = c;
  if (b.oom) {
    free(b.p);
    return NULL;
  }
  if (b.p == NULL)
    return strdup("");
  return b.p;
}

static bool numeric_token(const char *s) {
  bool digit = false;
  const char *c = s;
  if (*c == '+' || *c == '-')
    c++;
  for (; *c; c++) {
    if (isdigit((unsigned char)*c))
      digit = true;
    else if (*c != '.' && *c != 'e' && *c != 'E' && *c != '+' && *c != '-')
      return false;
  }
  return digit;
}

/* parse a plain/quoted scalar token (already trimmed) into a value. */
static hcl2_value *parse_scalar(const char *s, struct yp *p) {
  if (*s == '"' || *s == '\'') {
    const char *c = s;
    char *str = parse_quoted(&c, p);
    if (str == NULL)
      return NULL;
    while (*c == ' ')
      c++;
    if (*c != '\0') {
      free(str);
      return fail(p, "yamldecode: trailing data after quoted scalar");
    }
    hcl2_value *v = hcl2_string(str);
    free(str);
    return v;
  }
  if (s[0] == '\0' || strcmp(s, "null") == 0 || strcmp(s, "~") == 0)
    return hcl2_null();
  if (strcmp(s, "true") == 0)
    return hcl2_bool(true);
  if (strcmp(s, "false") == 0)
    return hcl2_bool(false);
  if (numeric_token(s)) {
    char *end;
    double d = strtod(s, &end);
    if (*end == '\0')
      return hcl2_number(d);
  }
  return hcl2_string(s);
}

/* ---- flow collections -------------------------------------------------- */

static hcl2_value *flow_value(const char **s, struct yp *p);

static void skipws(const char **s) {
  while (**s == ' ' || **s == '\t')
    (*s)++;
}

static char *flow_plain(const char **s) { /* until , ] } : */
  const char *start = *s;
  const char *c = *s;
  while (*c && *c != ',' && *c != ']' && *c != '}' && *c != ':')
    c++;
  const char *endp = c;
  while (endp > start && (endp[-1] == ' ' || endp[-1] == '\t'))
    endp--;
  *s = c;
  size_t len = (size_t)(endp - start);
  char *r = malloc(len + 1);
  if (r == NULL)
    return NULL;
  memcpy(r, start, len);
  r[len] = '\0';
  return r;
}

static hcl2_value *flow_seq(const char **s, struct yp *p) {
  (*s)++; /* '[' */
  hcl2_value *arr = hcl2_tuple();
  if (arr == NULL)
    return NULL;
  skipws(s);
  if (**s == ']') {
    (*s)++;
    return arr;
  }
  for (;;) {
    hcl2_value *v = flow_value(s, p);
    if (v == NULL || !hcl2_tuple_push(arr, v)) {
      hcl2_value_free(v);
      hcl2_value_free(arr);
      return p->failed ? NULL : NULL;
    }
    skipws(s);
    if (**s == ',') {
      (*s)++;
      skipws(s);
      if (**s == ']') { /* trailing comma */
        (*s)++;
        return arr;
      }
      continue;
    }
    if (**s == ']') {
      (*s)++;
      return arr;
    }
    hcl2_value_free(arr);
    return fail(p, "yamldecode: malformed flow sequence");
  }
}

static hcl2_value *flow_map(const char **s, struct yp *p) {
  (*s)++; /* '{' */
  hcl2_value *obj = hcl2_object();
  if (obj == NULL)
    return NULL;
  skipws(s);
  if (**s == '}') {
    (*s)++;
    return obj;
  }
  for (;;) {
    skipws(s);
    char *key;
    if (**s == '"' || **s == '\'')
      key = parse_quoted(s, p);
    else
      key = flow_plain(s);
    if (key == NULL) {
      hcl2_value_free(obj);
      return NULL;
    }
    skipws(s);
    if (**s != ':') {
      free(key);
      hcl2_value_free(obj);
      return fail(p, "yamldecode: expected ':' in flow mapping");
    }
    (*s)++;
    skipws(s);
    hcl2_value *v = flow_value(s, p);
    if (v == NULL || !hcl2_object_set(obj, key, v)) {
      free(key);
      hcl2_value_free(v);
      hcl2_value_free(obj);
      return NULL;
    }
    free(key);
    skipws(s);
    if (**s == ',') {
      (*s)++;
      skipws(s);
      if (**s == '}') {
        (*s)++;
        return obj;
      }
      continue;
    }
    if (**s == '}') {
      (*s)++;
      return obj;
    }
    hcl2_value_free(obj);
    return fail(p, "yamldecode: malformed flow mapping");
  }
}

static hcl2_value *flow_value(const char **s, struct yp *p) {
  skipws(s);
  char *anchor = NULL;
  int tag = TAG_NONE;
  parse_prefix(s, &anchor, &tag);
  skipws(s);
  hcl2_value *v;
  if (**s == '*') { /* alias */
    (*s)++;
    const char *n = *s;
    while (**s && **s != ',' && **s != ']' && **s != '}' && **s != ' ')
      (*s)++;
    v = anchor_lookup(p, n, (size_t)(*s - n));
    if (v == NULL) {
      free(anchor);
      return fail(p, "yamldecode: undefined alias");
    }
  } else if (**s == '[') {
    v = flow_seq(s, p);
  } else if (**s == '{') {
    v = flow_map(s, p);
  } else if (**s == '"' || **s == '\'') {
    char *str = parse_quoted(s, p);
    if (str == NULL) {
      free(anchor);
      return NULL;
    }
    v = hcl2_string(str);
    free(str);
  } else {
    char *tok = flow_plain(s);
    if (tok == NULL) {
      free(anchor);
      return NULL;
    }
    v = parse_scalar(tok, p);
    free(tok);
  }
  v = apply_tag(v, tag);
  if (v != NULL && anchor != NULL)
    anchor_register(p, anchor, v);
  free(anchor);
  return v;
}

/* parse a single inline value token (anchor/tag prefix already stripped): an
 * alias, a flow collection, or a scalar. */
static hcl2_value *parse_token(const char *s, struct yp *p) {
  while (*s == ' ')
    s++;
  if (*s == '*') { /* alias */
    const char *n = s + 1, *e = n;
    while (*e && *e != ' ')
      e++;
    hcl2_value *v = anchor_lookup(p, n, (size_t)(e - n));
    return v ? v : fail(p, "yamldecode: undefined alias");
  }
  if (*s == '[' || *s == '{') {
    const char *c = s;
    return flow_value(&c, p);
  }
  return parse_scalar(s, p);
}

/* ---- block structure --------------------------------------------------- */

static bool is_seq(const char *t) { return t[0] == '-' && (t[1] == ' ' || t[1] == '\0'); }

/* index of the key/value separating colon, or -1 if the line is not a map. */
static int map_colon(const char *t) {
  int depth = 0;
  char q = 0;
  for (int i = 0; t[i]; i++) {
    char c = t[i];
    if (q) {
      if (c == q)
        q = 0;
      continue;
    }
    if (c == '"' || c == '\'')
      q = c;
    else if (c == '[' || c == '{')
      depth++;
    else if (c == ']' || c == '}')
      depth--;
    else if (c == ':' && depth == 0 && (t[i + 1] == ' ' || t[i + 1] == '\0'))
      return i;
  }
  return -1;
}

static hcl2_value *parse_node_at(struct yp *p, int ind);

static void skip_blank(struct yp *p) {
  while (p->pos < p->n && p->lines[p->pos].text[0] == '\0')
    p->pos++;
}

/* value following a "key:" with empty RHS, given the key's indent. */
static hcl2_value *value_for_key(struct yp *p, int keyind) {
  skip_blank(p);
  if (p->pos >= p->n)
    return hcl2_null();
  int ind = p->lines[p->pos].indent;
  if (ind > keyind)
    return parse_node_at(p, ind);
  if (ind == keyind && is_seq(p->lines[p->pos].text))
    return parse_node_at(p, ind); /* sequence aligned with its key */
  return hcl2_null();
}

/* YAML merge key (<<): fold the keys of src (a map, or a sequence of maps in
 * decreasing precedence) into obj, without overriding keys already present --
 * so explicit keys always win over merged ones. */
static bool merge_into(hcl2_value *obj, const hcl2_value *src) {
  if (hcl2_is_keyed(src->kind)) {
    for (size_t i = 0; i < src->nf; i++) {
      const char *k = src->fields[i].key;
      if (hcl2_value_get(obj, k) != NULL)
        continue;
      hcl2_value *cv = vclone(src->fields[i].val);
      if (cv == NULL || !hcl2_object_set(obj, k, cv)) {
        hcl2_value_free(cv);
        return false;
      }
    }
    return true;
  }
  if (hcl2_is_seq(src->kind)) {
    for (size_t i = 0; i < src->n; i++) /* earlier entries take precedence */
      if (!merge_into(obj, src->items[i]))
        return false;
    return true;
  }
  return false; /* a << value must be a map or a sequence of maps */
}

static hcl2_value *parse_map(struct yp *p, int ind) {
  hcl2_value *obj = hcl2_object();
  if (obj == NULL)
    return NULL;
  while (p->pos < p->n && p->lines[p->pos].indent == ind && !is_seq(p->lines[p->pos].text)) {
    char *t = p->lines[p->pos].text;
    int col = map_colon(t);
    if (col < 0) {
      hcl2_value_free(obj);
      return fail(p, "yamldecode: expected mapping key");
    }
    /* key text is t[0..col), trimmed */
    char saved = t[col];
    t[col] = '\0';
    char *kt = t;
    while (*kt == ' ')
      kt++;
    char *kend = t + col;
    while (kend > kt && kend[-1] == ' ')
      kend--;
    *kend = '\0';
    char *key;
    if (*kt == '"' || *kt == '\'') {
      const char *kc = kt;
      key = parse_quoted(&kc, p);
    } else {
      key = strdup(kt);
    }
    t[col] = saved;
    if (key == NULL) {
      hcl2_value_free(obj);
      return NULL;
    }
    const char *rhs = t + col + 1;
    while (*rhs == ' ')
      rhs++;
    p->pos++;
    char *anchor = NULL;
    int tag = TAG_NONE;
    const char *rp = rhs;
    parse_prefix(&rp, &anchor, &tag);
    hcl2_value *v = (*rp == '\0') ? value_for_key(p, ind) : parse_token(rp, p);
    v = apply_tag(v, tag);
    if (v != NULL && anchor != NULL)
      anchor_register(p, anchor, v);
    free(anchor);
    if (v != NULL && strcmp(key, "<<") == 0) { /* merge key */
      bool ok = merge_into(obj, v);
      hcl2_value_free(v);
      free(key);
      if (!ok) {
        hcl2_value_free(obj);
        return fail(p, "yamldecode: << merge value must be a map or list of maps");
      }
      continue;
    }
    if (v == NULL || !hcl2_object_set(obj, key, v)) {
      free(key);
      hcl2_value_free(v);
      hcl2_value_free(obj);
      return NULL;
    }
    free(key);
  }
  return obj;
}

static hcl2_value *parse_seq(struct yp *p, int ind) {
  hcl2_value *arr = hcl2_tuple();
  if (arr == NULL)
    return NULL;
  while (p->pos < p->n && p->lines[p->pos].indent == ind && is_seq(p->lines[p->pos].text)) {
    char *full = p->lines[p->pos].text;
    int col = ind + 1;
    char *c = full + 1;
    while (*c == ' ') {
      c++;
      col++;
    }
    hcl2_value *item;
    if (*c == '\0') { /* nested block on following deeper lines */
      p->pos++;
      skip_blank(p);
      if (p->pos < p->n && p->lines[p->pos].indent > ind)
        item = parse_node_at(p, p->lines[p->pos].indent);
      else
        item = hcl2_null();
    } else if (*c == '*' || *c == '&' || *c == '!') {
      /* alias / anchored / tagged item (not the inline-map "- key: v" form) */
      char *anchor = NULL;
      int tag = TAG_NONE;
      const char *cp = c;
      parse_prefix(&cp, &anchor, &tag);
      if (*cp == '\0') { /* anchor/tag then a nested block */
        p->pos++;
        skip_blank(p);
        if (p->pos < p->n && p->lines[p->pos].indent > ind)
          item = parse_node_at(p, p->lines[p->pos].indent);
        else
          item = hcl2_null();
      } else {
        item = parse_token(cp, p);
        p->pos++;
      }
      item = apply_tag(item, tag);
      if (item != NULL && anchor != NULL)
        anchor_register(p, anchor, item);
      free(anchor);
    } else {
      /* rewrite the current line so its content sits at column `col`,
         then parse a node there (handles "- key: v" inline maps). */
      p->lines[p->pos].indent = col;
      p->lines[p->pos].text = c;
      item = parse_node_at(p, col);
    }
    if (item == NULL || !hcl2_tuple_push(arr, item)) {
      hcl2_value_free(item);
      hcl2_value_free(arr);
      return NULL;
    }
  }
  return arr;
}

static hcl2_value *parse_node_at(struct yp *p, int ind) {
  char *t = p->lines[p->pos].text;
  if (t[0] == '[' || t[0] == '{') {
    const char *c = t;
    hcl2_value *v = flow_value(&c, p);
    p->pos++;
    return v;
  }
  if (is_seq(t))
    return parse_seq(p, ind);
  if (map_colon(t) >= 0)
    return parse_map(p, ind);
  hcl2_value *v = parse_scalar(t, p);
  p->pos++;
  return v;
}

/* append bytes [p, p+n) to a double-quoted scalar, escaping for the parser. */
static void emit_escaped(struct sb *out, const char *p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    switch (c) {
    case '"':
      sb_puts(out, "\\\"");
      break;
    case '\\':
      sb_puts(out, "\\\\");
      break;
    case '\n':
      sb_puts(out, "\\n");
      break;
    case '\t':
      sb_puts(out, "\\t");
      break;
    case '\r':
      sb_puts(out, "\\r");
      break;
    default:
      if (c < 0x20) {
        char b[8];
        snprintf(b, sizeof b, "\\u%04x", c);
        sb_puts(out, b);
      } else {
        sb_put(out, (const char *)&c, 1);
      }
    }
  }
}

/* Pre-pass: rewrite YAML block scalars (key: | / key: >, with optional -/+
 * chomping) into equivalent double-quoted single-line scalars, so the
 * line-based parser below can stay simple. Conservative: only clear block
 * headers are rewritten; every other line is copied verbatim. Returns a new
 * NUL-terminated source string (caller frees) or NULL on OOM. */
static char *yaml_unblock(const char *src) {
  struct bl {
    const char *p;
    size_t len;
    bool blank;
  };
  struct sb out = {0};
  const char *line = src;
  while (*line) {
    const char *nl = strchr(line, '\n');
    size_t linelen = nl ? (size_t)(nl - line) : strlen(line);
    size_t ind = 0;
    while (ind < linelen && line[ind] == ' ')
      ind++;
    const char *content = line + ind;
    size_t ct = linelen - ind;
    while (ct > 0 && (content[ct - 1] == ' ' || content[ct - 1] == '\t' || content[ct - 1] == '\r'))
      ct--;
    /* locate the value token (after a mapping ':' or a sequence '- ') */
    const char *val = NULL;
    for (size_t i = 0; i < ct; i++)
      if (content[i] == ':' && (i + 1 == ct || content[i + 1] == ' ')) {
        val = content + i + 1;
        break;
      }
    if (val == NULL) {
      if (ct >= 1 && content[0] == '-' && (ct == 1 || content[1] == ' '))
        val = content + 1;
      else
        val = content;
    }
    while (val < content + ct && *val == ' ')
      val++;
    size_t vlen = (size_t)((content + ct) - val);
    int style = 0, chomp = 0;
    if (vlen >= 1 && (val[0] == '|' || val[0] == '>')) {
      bool ok = true;
      int ch = 0;
      for (size_t i = 1; i < vlen; i++) {
        char d = val[i];
        if (d == '-' || d == '+') {
          if (ch) {
            ok = false;
            break;
          }
          ch = d;
        } else if (d < '0' || d > '9') {
          ok = false;
          break;
        }
      }
      if (ok) {
        style = val[0];
        chomp = ch;
      }
    }
    if (style == 0) { /* not a block header: copy the line verbatim */
      sb_put(&out, line, linelen);
      if (nl)
        sb_put(&out, "\n", 1);
      line = nl ? nl + 1 : line + linelen;
      continue;
    }
    size_t prefixlen = (size_t)(val - content);
    const char *body = nl ? nl + 1 : line + linelen;
    /* block indent = indent of the first non-blank body line (must exceed ind) */
    size_t blockindent = 0;
    bool have = false;
    for (const char *scan = body; *scan;) {
      const char *snl = strchr(scan, '\n');
      size_t sl = snl ? (size_t)(snl - scan) : strlen(scan);
      size_t si = 0;
      while (si < sl && scan[si] == ' ')
        si++;
      if (si != sl) {
        blockindent = si;
        have = si > ind;
        break;
      }
      scan = snl ? snl + 1 : scan + sl;
    }
    struct bl *arr = NULL;
    size_t na = 0, ca = 0;
    const char *b = body;
    if (have) {
      while (*b) {
        const char *bnl = strchr(b, '\n');
        size_t blen = bnl ? (size_t)(bnl - b) : strlen(b);
        size_t bi = 0;
        while (bi < blen && b[bi] == ' ')
          bi++;
        bool blank = (bi == blen);
        if (!blank && bi < blockindent)
          break; /* dedent below the block: done */
        size_t drop = blank ? (blen < blockindent ? blen : blockindent) : blockindent;
        if (na == ca) {
          ca = ca ? ca * 2 : 8;
          struct bl *nr = realloc(arr, ca * sizeof *arr);
          if (nr == NULL) {
            free(arr);
            free(out.p);
            return NULL;
          }
          arr = nr;
        }
        arr[na].p = b + drop;
        arr[na].len = blen - drop;
        arr[na].blank = blank;
        na++;
        b = bnl ? bnl + 1 : b + blen;
      }
    }
    const char *resume = have ? b : body;
    size_t end = na; /* trailing blank lines handled by chomping */
    while (end > 0 && arr[end - 1].blank)
      end--;
    size_t trailing = na - end;
    /* emit: <indent><prefix>"<folded/literal content>" */
    sb_put(&out, line, ind);
    sb_put(&out, content, prefixlen);
    sb_put(&out, "\"", 1);
    if (style == '|') {
      for (size_t i = 0; i < end; i++) {
        if (i)
          sb_puts(&out, "\\n");
        emit_escaped(&out, arr[i].p, arr[i].len);
      }
    } else { /* '>' folded */
      bool prevtext = false;
      for (size_t i = 0; i < end; i++) {
        if (arr[i].blank) {
          sb_puts(&out, "\\n");
          prevtext = false;
        } else {
          if (prevtext)
            sb_puts(&out, " ");
          emit_escaped(&out, arr[i].p, arr[i].len);
          prevtext = true;
        }
      }
    }
    if (end > 0) { /* chomping: clip (default), strip '-', keep '+' */
      if (chomp == '+') {
        for (size_t i = 0; i < trailing; i++)
          sb_puts(&out, "\\n");
        sb_puts(&out, "\\n");
      } else if (chomp != '-') {
        sb_puts(&out, "\\n");
      }
    }
    sb_put(&out, "\"", 1);
    sb_put(&out, "\n", 1);
    free(arr);
    line = resume;
  }
  sb_put(&out, "", 0);
  if (out.oom) {
    free(out.p);
    return NULL;
  }
  if (out.p == NULL)
    return strdup("");
  /* NUL-terminate (sb stores raw bytes) */
  char *r = realloc(out.p, out.len + 1);
  if (r == NULL) {
    free(out.p);
    return NULL;
  }
  r[out.len] = '\0';
  return r;
}

hcl2_value *bi_yamldecode(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 1 || a[0]->kind != HCL2_STRING) {
    everr(e, es, "yamldecode() needs (string)");
    return NULL;
  }
  char *src = yaml_unblock(a[0]->str);
  if (src == NULL)
    return NULL;
  struct yp p = {0};
  p.err = e;
  p.errsz = es;
  size_t cap = 16;
  p.lines = malloc(cap * sizeof(*p.lines));
  if (p.lines == NULL) {
    free(src);
    return NULL;
  }
  /* document start line-indices (--- / ... separators); the first document
   * implicitly starts at line 0. */
  size_t cstarts = 8, nstarts = 1;
  size_t *starts = malloc(cstarts * sizeof *starts);
  if (starts == NULL) {
    free(src);
    free(p.lines);
    return NULL;
  }
  starts[0] = 0;
  /* split into comment-stripped, trimmed, non-blank lines */
  char *line = src;
  while (line != NULL && *line != '\0') {
    char *nl = strchr(line, '\n');
    if (nl != NULL)
      *nl = '\0';
    /* indentation */
    int indent = 0;
    char *t = line;
    while (*t == ' ') {
      t++;
      indent++;
    }
    /* strip a trailing comment (# preceded by space/start, outside quotes) */
    char q = 0;
    for (char *c = t; *c; c++) {
      if (q) {
        if (*c == q)
          q = 0;
      } else if (*c == '"' || *c == '\'')
        q = *c;
      else if (*c == '#' && (c == t || c[-1] == ' ')) {
        *c = '\0';
        break;
      }
    }
    /* trailing trim */
    char *end = t + strlen(t);
    while (end > t && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
      end--;
    *end = '\0';
    if (strcmp(t, "---") == 0 || strcmp(t, "...") == 0) {
      /* document separator: begin a new document if the current one has lines */
      if (p.n > starts[nstarts - 1]) {
        if (nstarts == cstarts) {
          cstarts *= 2;
          size_t *ns = realloc(starts, cstarts * sizeof *ns);
          if (ns == NULL) {
            free(src);
            free(p.lines);
            free(starts);
            return NULL;
          }
          starts = ns;
        }
        starts[nstarts++] = p.n;
      }
    } else if (*t != '\0') {
      if (p.n == cap) {
        cap *= 2;
        struct yline *nlines = realloc(p.lines, cap * sizeof(*p.lines));
        if (nlines == NULL) {
          free(src);
          free(p.lines);
          free(starts);
          return NULL;
        }
        p.lines = nlines;
      }
      p.lines[p.n].indent = indent;
      p.lines[p.n].text = t;
      p.n++;
    }
    line = (nl != NULL) ? nl + 1 : NULL;
  }
  /* parse each non-empty document range: a single document yields its value
   * (Terraform-compatible), several yield a tuple of the documents. Anchors are
   * scoped per document. */
  size_t total = p.n;
  hcl2_value *single = NULL; /* the first/only document */
  hcl2_value *tup = NULL;    /* created once a second document appears */
  size_t ndocs = 0;
  bool ok = true;
  for (size_t di = 0; di < nstarts && ok; di++) {
    size_t ds = starts[di];
    size_t de = (di + 1 < nstarts) ? starts[di + 1] : total;
    if (ds >= de)
      continue; /* empty range (e.g. a trailing separator) */
    for (size_t i = 0; i < p.nanch; i++) {
      free(p.anch[i].name);
      hcl2_value_free(p.anch[i].val);
    }
    p.nanch = 0;
    p.pos = ds;
    p.n = de; /* bound the parser to this document */
    hcl2_value *dv = parse_node_at(&p, p.lines[ds].indent);
    if (dv == NULL || p.failed) {
      hcl2_value_free(dv);
      ok = false;
      break;
    }
    if (p.pos != de) {
      hcl2_value_free(dv);
      everr(e, es, "yamldecode: trailing content");
      ok = false;
      break;
    }
    if (ndocs == 0) {
      single = dv;
    } else {
      if (tup == NULL) { /* promote to a tuple of documents */
        tup = hcl2_tuple();
        if (tup == NULL || !hcl2_tuple_push(tup, single)) {
          hcl2_value_free(tup);
          hcl2_value_free(single);
          hcl2_value_free(dv);
          single = NULL;
          tup = NULL;
          ok = false;
          break;
        }
        single = NULL;
      }
      if (!hcl2_tuple_push(tup, dv)) {
        hcl2_value_free(dv);
        ok = false;
        break;
      }
    }
    ndocs++;
  }
  hcl2_value *r;
  if (!ok) {
    hcl2_value_free(single);
    hcl2_value_free(tup);
    r = NULL;
  } else if (ndocs == 0) {
    r = hcl2_null();
  } else {
    r = tup != NULL ? tup : single;
  }
  free(starts);
  free(src);
  free(p.lines);
  for (size_t i = 0; i < p.nanch; i++) {
    free(p.anch[i].name);
    hcl2_value_free(p.anch[i].val);
  }
  free(p.anch);
  return r;
}

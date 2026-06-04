#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * JSON body profile (M5) -- schema-driven decoding
 *
 * A JSON object is an HCL *body*, but JSON cannot distinguish an attribute from
 * a (labeled) block on its own, so the caller supplies a schema. We turn the
 * JSON document into the very same hcl2_doc/hcl2_body tree the native parser
 * builds: each attribute value is synthesized into an expression AST node
 * (HCL2 JSON strings are templates -> N_TEMPLATE in heredoc mode, so they are
 * evaluated lazily against a context just like native attributes), and blocks
 * are descended `nlabels` deep before their bodies are decoded against the
 * child schema. Once built, every existing accessor and hcl2_body_attr_value
 * works unchanged.
 * ===========================================================================*/

struct attr_spec {
  char *name;
  bool required;
};
struct block_spec {
  char *type;
  size_t nlabels;
  hcl2_schema *child; /* owned; NULL == empty body */
};
struct hcl2_schema {
  struct attr_spec *attrs;
  size_t nattr;
  struct block_spec *blocks;
  size_t nblock;
};

/* --- schema construction --- */
hcl2_schema *hcl2_schema_new(void) { return calloc(1, sizeof(hcl2_schema)); }

bool hcl2_schema_attr(hcl2_schema *s, const char *name, bool required) {
  if (s == NULL || name == NULL)
    return false;
  struct attr_spec *na = realloc(s->attrs, (s->nattr + 1) * sizeof(*na));
  if (na == NULL)
    return false;
  s->attrs = na;
  s->attrs[s->nattr].name = strdup(name);
  if (s->attrs[s->nattr].name == NULL)
    return false;
  s->attrs[s->nattr].required = required;
  s->nattr++;
  return true;
}

bool hcl2_schema_block(hcl2_schema *s, const char *type, size_t nlabels, hcl2_schema *child) {
  if (s == NULL || type == NULL) {
    hcl2_schema_free(child); /* consume ownership even on failure */
    return false;
  }
  struct block_spec *nb = realloc(s->blocks, (s->nblock + 1) * sizeof(*nb));
  if (nb == NULL) {
    hcl2_schema_free(child);
    return false;
  }
  s->blocks = nb;
  s->blocks[s->nblock].type = strdup(type);
  if (s->blocks[s->nblock].type == NULL) {
    hcl2_schema_free(child);
    return false;
  }
  s->blocks[s->nblock].nlabels = nlabels;
  s->blocks[s->nblock].child = child;
  s->nblock++;
  return true;
}

void hcl2_schema_free(hcl2_schema *s) {
  if (s == NULL)
    return;
  for (size_t i = 0; i < s->nattr; i++)
    free(s->attrs[i].name);
  free(s->attrs);
  for (size_t i = 0; i < s->nblock; i++) {
    free(s->blocks[i].type);
    hcl2_schema_free(s->blocks[i].child);
  }
  free(s->blocks);
  free(s);
}

/* --- JSON value -> expression AST node --- */

static struct node *jnode(const hcl2_value *v);

static struct node *nlit(hcl2_value *val /* owned */) {
  if (val == NULL)
    return NULL;
  struct node *n = calloc(1, sizeof(*n));
  if (n == NULL) {
    hcl2_value_free(val);
    return NULL;
  }
  n->kind = N_LIT;
  n->lit = val;
  return n;
}

/* A JSON string becomes a heredoc-mode template node: ${ } / %{ } expand at eval
 * time, backslashes stay literal (JSON already un-escaped them). */
static struct node *ntemplate(const char *s) {
  struct node *n = calloc(1, sizeof(*n));
  if (n == NULL)
    return NULL;
  n->kind = N_TEMPLATE;
  n->op = T_HEREDOC;
  n->str = strdup(s != NULL ? s : "");
  if (n->str == NULL) {
    free(n);
    return NULL;
  }
  return n;
}

static struct node *jnode(const hcl2_value *v) {
  switch (hcl2_value_kind(v)) {
  case HCL2_STRING:
    return ntemplate(v->str);
  case HCL2_NUMBER:
    return nlit(hcl2_number(v->num));
  case HCL2_BOOL:
    return nlit(hcl2_bool(v->b));
  case HCL2_NULL:
    return nlit(hcl2_null());
  case HCL2_UNKNOWN:
    return nlit(hcl2_unknown());
  default:
    break; /* a collection */
  }
  struct node *n = calloc(1, sizeof(*n));
  if (n == NULL)
    return NULL;
  if (hcl2_is_seq(v->kind)) {
    n->kind = N_TUPLE;
    if (v->n > 0) {
      n->items = calloc(v->n, sizeof(*n->items));
      if (n->items == NULL) {
        free(n);
        return NULL;
      }
      n->n = v->n;
      for (size_t i = 0; i < v->n; i++)
        if ((n->items[i] = jnode(v->items[i])) == NULL) {
          node_free(n);
          return NULL;
        }
    }
    return n;
  }
  /* keyed: object/map -> N_OBJECT with literal string keys */
  n->kind = N_OBJECT;
  if (v->nf > 0) {
    n->items = calloc(v->nf, sizeof(*n->items));
    n->keys = calloc(v->nf, sizeof(*n->keys));
    if (n->items == NULL || n->keys == NULL) {
      node_free(n);
      return NULL;
    }
    n->n = v->nf;
    for (size_t i = 0; i < v->nf; i++) {
      n->keys[i] = strdup(v->fields[i].key);
      if (n->keys[i] == NULL || (n->items[i] = jnode(v->fields[i].val)) == NULL) {
        node_free(n);
        return NULL;
      }
    }
  }
  return n;
}

/* --- body assembly helpers --- */

static bool add_attr(struct hcl2_body *b, const char *name, struct node *expr) {
  struct hcl2_attr *a = calloc(1, sizeof(*a));
  if (a == NULL) {
    node_free(expr);
    return false;
  }
  a->name = strdup(name);
  if (a->name == NULL) {
    node_free(expr);
    free(a);
    return false;
  }
  a->expr = expr;
  struct hcl2_attr **na = realloc(b->attrs, (b->nattr + 1) * sizeof(*na));
  if (na == NULL) {
    node_free(expr);
    free(a->name);
    free(a);
    return false;
  }
  b->attrs = na;
  b->attrs[b->nattr++] = a;
  return true;
}

static bool add_block(struct hcl2_body *b, struct hcl2_block *bl) {
  struct hcl2_block **nb = realloc(b->blocks, (b->nblock + 1) * sizeof(*nb));
  if (nb == NULL)
    return false;
  b->blocks = nb;
  b->blocks[b->nblock++] = bl;
  return true;
}

/* Free a standalone block (body.c's block_free is static; this mirrors it for
 * the decoder's error paths). */
static void free_block(struct hcl2_block *bl) {
  if (bl == NULL)
    return;
  free(bl->type);
  for (size_t i = 0; i < bl->nlabel; i++)
    free(bl->labels[i]);
  free(bl->labels);
  hcl2_body_free(bl->body);
  free(bl);
}

static struct hcl2_body *decode_body(const hcl2_value *obj, const hcl2_schema *schema, char *err,
                                     size_t errsz);

/* Build one hcl2_block with a copy of `labels` and a body decoded from `obj`. */
static struct hcl2_block *make_block(const char *type, char *const *labels, size_t nlabels,
                                     const hcl2_value *obj, const hcl2_schema *child, char *err,
                                     size_t errsz) {
  struct hcl2_block *bl = calloc(1, sizeof(*bl));
  if (bl == NULL)
    return NULL;
  bl->type = strdup(type);
  if (bl->type == NULL) {
    free(bl);
    return NULL;
  }
  if (nlabels > 0) {
    bl->labels = calloc(nlabels, sizeof(*bl->labels));
    if (bl->labels == NULL) {
      free_block(bl);
      return NULL;
    }
    bl->nlabel = nlabels;
    for (size_t i = 0; i < nlabels; i++) {
      bl->labels[i] = strdup(labels[i]);
      if (bl->labels[i] == NULL) {
        free_block(bl);
        return NULL;
      }
    }
  }
  bl->body = decode_body(obj, child, err, errsz);
  if (bl->body == NULL) {
    free_block(bl);
    return NULL;
  }
  return bl;
}

/* Descend `nremaining` label levels, then decode the block body (or array of
 * bodies) at the leaf. `labels` holds the labels collected so far. */
static bool decode_blocks(const hcl2_value *val, const char *type, char **labels, size_t nfilled,
                          size_t nremaining, const hcl2_schema *child, struct hcl2_body *out,
                          char *err, size_t errsz) {
  if (nremaining > 0) {
    if (!hcl2_is_keyed(val->kind)) {
      everr(err, errsz, "json: expected an object for block labels");
      return false;
    }
    for (size_t i = 0; i < val->nf; i++) {
      labels[nfilled] = val->fields[i].key; /* borrow; copied in make_block */
      if (!decode_blocks(val->fields[i].val, type, labels, nfilled + 1, nremaining - 1, child, out,
                         err, errsz))
        return false;
    }
    return true;
  }
  /* leaf: a single object is one block; an array is several blocks. */
  if (hcl2_is_keyed(val->kind)) {
    struct hcl2_block *bl = make_block(type, labels, nfilled, val, child, err, errsz);
    if (bl == NULL || !add_block(out, bl)) {
      free_block(bl);
      return false;
    }
    return true;
  }
  if (hcl2_is_seq(val->kind)) {
    for (size_t i = 0; i < val->n; i++) {
      const hcl2_value *e = val->items[i];
      if (!hcl2_is_keyed(e->kind)) {
        everr(err, errsz, "json: each block in an array must be an object");
        return false;
      }
      struct hcl2_block *bl = make_block(type, labels, nfilled, e, child, err, errsz);
      if (bl == NULL || !add_block(out, bl)) {
        free_block(bl);
        return false;
      }
    }
    return true;
  }
  everr(err, errsz, "json: a block body must be an object or array of objects");
  return false;
}

static bool name_is_known(const hcl2_schema *schema, const char *key) {
  for (size_t i = 0; i < schema->nattr; i++)
    if (strcmp(schema->attrs[i].name, key) == 0)
      return true;
  for (size_t i = 0; i < schema->nblock; i++)
    if (strcmp(schema->blocks[i].type, key) == 0)
      return true;
  return false;
}

static struct hcl2_body *decode_body(const hcl2_value *obj, const hcl2_schema *schema, char *err,
                                     size_t errsz) {
  static const hcl2_schema empty = {0};
  if (schema == NULL)
    schema = &empty;
  if (!hcl2_is_keyed(obj->kind)) {
    everr(err, errsz, "json: a body must be a JSON object");
    return NULL;
  }
  struct hcl2_body *b = calloc(1, sizeof(*b));
  if (b == NULL)
    return NULL;

  /* attributes */
  for (size_t i = 0; i < schema->nattr; i++) {
    const hcl2_value *av = hcl2_value_get(obj, schema->attrs[i].name);
    if (av == NULL) {
      if (schema->attrs[i].required) {
        everr(err, errsz, "json: missing required attribute");
        hcl2_body_free(b);
        return NULL;
      }
      continue;
    }
    struct node *expr = jnode(av);
    if (expr == NULL || !add_attr(b, schema->attrs[i].name, expr)) {
      hcl2_body_free(b);
      return NULL;
    }
  }

  /* blocks */
  for (size_t i = 0; i < schema->nblock; i++) {
    const hcl2_value *bv = hcl2_value_get(obj, schema->blocks[i].type);
    if (bv == NULL)
      continue;
    char **labels = NULL;
    if (schema->blocks[i].nlabels > 0) {
      labels = calloc(schema->blocks[i].nlabels, sizeof(*labels));
      if (labels == NULL) {
        hcl2_body_free(b);
        return NULL;
      }
    }
    bool ok = decode_blocks(bv, schema->blocks[i].type, labels, 0, schema->blocks[i].nlabels,
                            schema->blocks[i].child, b, err, errsz);
    free(labels);
    if (!ok) {
      hcl2_body_free(b);
      return NULL;
    }
  }

  /* reject unrecognized properties */
  for (size_t i = 0; i < obj->nf; i++) {
    if (!name_is_known(schema, obj->fields[i].key)) {
      everr(err, errsz, "json: unsupported property (not in schema)");
      hcl2_body_free(b);
      return NULL;
    }
  }
  return b;
}

hcl2_doc *hcl2_json_decode(const char *src, size_t len, const hcl2_schema *schema, char *err,
                           size_t errsz) {
  if (err != NULL && errsz > 0)
    err[0] = '\0';
  hcl2_value *root = hcl2_parse_json(src, len, err, errsz);
  if (root == NULL)
    return NULL;
  struct hcl2_body *body = decode_body(root, schema, err, errsz);
  hcl2_value_free(root);
  if (body == NULL)
    return NULL;
  hcl2_doc *doc = calloc(1, sizeof(*doc));
  if (doc == NULL) {
    hcl2_body_free(body);
    return NULL;
  }
  doc->root = body;
  return doc;
}

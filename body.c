#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * Configuration bodies (M2)
 *
 * A document is a body: a sequence of attributes (`name = expr`) and blocks
 * (`type "label"... { body }`). Attribute expressions are stored unevaluated
 * and decoded lazily against a caller-supplied context, which is the whole
 * point of HCL2 over a plain config subset.
 * ===========================================================================*/

static bool push(void ***arr, size_t *n, void *item) {
  void **na = realloc(*arr, (*n + 1) * sizeof(void *));
  if (na == NULL)
    return false;
  *arr = na;
  na[*n] = item;
  (*n)++;
  return true;
}

static void attr_free(struct hcl2_attr *a) {
  if (a == NULL)
    return;
  free(a->name);
  node_free(a->expr);
  free(a);
}

static void block_free(struct hcl2_block *bl) {
  if (bl == NULL)
    return;
  free(bl->type);
  for (size_t i = 0; i < bl->nlabel; i++)
    free(bl->labels[i]);
  free(bl->labels);
  hcl2_body_free(bl->body);
  free(bl);
}

void hcl2_body_free(struct hcl2_body *b) {
  if (b == NULL)
    return;
  for (size_t i = 0; i < b->nattr; i++)
    attr_free(b->attrs[i]);
  free(b->attrs);
  for (size_t i = 0; i < b->nblock; i++)
    block_free(b->blocks[i]);
  free(b->blocks);
  free(b);
}

/* Parse `name = expr` (the leading IDENT already consumed into `name`). Takes
 * ownership of `name` on every path. */
static bool parse_attr(struct parser *p, struct hcl2_body *b, char *name) {
  lex(&p->lx); /* consume '=' */
  struct node *expr = parse_expr(p);
  if (expr == NULL) {
    free(name);
    return false;
  }
  struct hcl2_attr *a = calloc(1, sizeof(*a));
  if (a == NULL) {
    node_free(expr);
    free(name);
    return false;
  }
  a->name = name; /* ownership transferred to the attr */
  a->expr = expr;
  if (!push((void ***)&b->attrs, &b->nattr, a)) {
    attr_free(a); /* frees name + expr */
    return false;
  }
  return true;
}

/* Parse `type label... { body }` (the leading IDENT already consumed into
 * `type`, and the current token is whatever followed it). Takes ownership of
 * `type` on every path. */
static bool parse_block(struct parser *p, struct hcl2_body *b, char *type) {
  struct lexer *l = &p->lx;
  struct hcl2_block *bl = calloc(1, sizeof(*bl));
  if (bl == NULL) {
    free(type);
    return false;
  }
  bl->type = type; /* ownership transferred to the block */
  while (l->tok == T_STR || l->tok == T_IDENT) {
    char *lab = strdup(l->text);
    if (lab == NULL || !push((void ***)&bl->labels, &bl->nlabel, lab)) {
      free(lab);
      block_free(bl);
      return false;
    }
    lex(l);
  }
  if (l->tok != T_LC) {
    lx_err(l, "expected '{' to begin a block body");
    block_free(bl);
    return false;
  }
  lex(l); /* consume '{' */
  bl->body = parse_body(p, false);
  if (bl->body == NULL) {
    block_free(bl);
    return false;
  }
  /* parse_body(false) returns with the current token at '}'. */
  lex(l); /* consume '}' */
  if (!push((void ***)&b->blocks, &b->nblock, bl)) {
    block_free(bl);
    return false;
  }
  return true;
}

struct hcl2_body *parse_body(struct parser *p, bool toplevel) {
  struct lexer *l = &p->lx;
  struct hcl2_body *b = calloc(1, sizeof(*b));
  if (b == NULL)
    return NULL;
  for (;;) {
    if (l->tok == T_EOF) {
      if (toplevel)
        return b;
      lx_err(l, "unexpected end of input (expected '}')");
      hcl2_body_free(b);
      return NULL;
    }
    if (l->tok == T_RC) {
      if (!toplevel)
        return b; /* leave '}' for the caller to consume */
      lx_err(l, "unexpected '}'");
      hcl2_body_free(b);
      return NULL;
    }
    if (l->tok != T_IDENT) {
      lx_err(l, "expected an attribute or block name");
      hcl2_body_free(b);
      return NULL;
    }
    char *name = strdup(l->text);
    if (name == NULL) {
      hcl2_body_free(b);
      return NULL;
    }
    lex(l); /* token after the name decides attribute vs block */
    /* parse_attr / parse_block take ownership of `name` on every path. */
    bool ok = (l->tok == T_ASSIGN) ? parse_attr(p, b, name) : parse_block(p, b, name);
    if (!ok) {
      hcl2_body_free(b);
      return NULL;
    }
  }
}

/* ===========================================================================
 * Public API
 * ===========================================================================*/
hcl2_doc *hcl2_parse(const char *src, size_t len, char *err, size_t errsz) {
  if (err != NULL && errsz > 0)
    err[0] = '\0';
  struct parser p = {0};
  p.lx.p = src;
  p.lx.end = src + len;
  p.lx.err = err;
  p.lx.errsz = errsz;
  lex(&p.lx);
  struct hcl2_body *root = parse_body(&p, true);
  free(p.lx.text);
  if (root == NULL) {
    everr(err, errsz, "parse error");
    return NULL;
  }
  hcl2_doc *doc = calloc(1, sizeof(*doc));
  if (doc == NULL) {
    hcl2_body_free(root);
    return NULL;
  }
  doc->root = root;
  return doc;
}

void hcl2_doc_free(hcl2_doc *doc) {
  if (doc == NULL)
    return;
  hcl2_body_free(doc->root);
  free(doc);
}

const hcl2_body *hcl2_doc_root(const hcl2_doc *doc) { return doc ? doc->root : NULL; }

size_t hcl2_body_attr_count(const hcl2_body *b) { return b ? b->nattr : 0; }

const char *hcl2_body_attr_name(const hcl2_body *b, size_t i) {
  if (b == NULL || i >= b->nattr)
    return NULL;
  return b->attrs[i]->name;
}

bool hcl2_body_has_attr(const hcl2_body *b, const char *name) {
  if (b == NULL)
    return false;
  for (size_t i = 0; i < b->nattr; i++)
    if (strcmp(b->attrs[i]->name, name) == 0)
      return true;
  return false;
}

hcl2_value *hcl2_body_attr_value(const hcl2_body *b, const char *name, hcl2_ctx *ctx, char *err,
                                 size_t errsz) {
  if (err != NULL && errsz > 0)
    err[0] = '\0';
  if (b != NULL) {
    for (size_t i = 0; i < b->nattr; i++)
      if (strcmp(b->attrs[i]->name, name) == 0)
        return hcl2_eval_node(b->attrs[i]->expr, ctx, err, errsz);
  }
  everr(err, errsz, "no such attribute");
  return NULL;
}

static bool block_matches(const struct hcl2_block *bl, const char *type) {
  return type == NULL || strcmp(bl->type, type) == 0;
}

size_t hcl2_body_block_count(const hcl2_body *b, const char *type) {
  if (b == NULL)
    return 0;
  size_t c = 0;
  for (size_t i = 0; i < b->nblock; i++)
    if (block_matches(b->blocks[i], type))
      c++;
  return c;
}

const hcl2_block *hcl2_body_block_at(const hcl2_body *b, const char *type, size_t idx) {
  if (b == NULL)
    return NULL;
  for (size_t i = 0; i < b->nblock; i++) {
    if (block_matches(b->blocks[i], type)) {
      if (idx == 0)
        return b->blocks[i];
      idx--;
    }
  }
  return NULL;
}

const char *hcl2_block_type(const hcl2_block *bl) { return bl ? bl->type : NULL; }

size_t hcl2_block_label_count(const hcl2_block *bl) { return bl ? bl->nlabel : 0; }

const char *hcl2_block_label(const hcl2_block *bl, size_t i) {
  if (bl == NULL || i >= bl->nlabel)
    return NULL;
  return bl->labels[i];
}

const hcl2_body *hcl2_block_body(const hcl2_block *bl) { return bl ? bl->body : NULL; }

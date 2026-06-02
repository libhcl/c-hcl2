#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"

/* ===========================================================================
 * Value model
 * ===========================================================================*/
struct kv {
  char *key;
  hcl2_value *val;
};
struct hcl2_value {
  hcl2_kind kind;
  bool b;
  double num;
  char *str;
  hcl2_value **items; /* tuple */
  size_t n;
  struct kv *fields; /* object */
  size_t nf;
};

static hcl2_value *vnew(hcl2_kind k) {
  hcl2_value *v = calloc(1, sizeof(*v));
  if (v != NULL)
    v->kind = k;
  return v;
}
hcl2_value *hcl2_null(void) { return vnew(HCL2_NULL); }
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

static hcl2_value *vclone(const hcl2_value *v) {
  if (v == NULL)
    return NULL;
  switch (v->kind) {
  case HCL2_NULL:
    return hcl2_null();
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

static bool vequal(const hcl2_value *a, const hcl2_value *b) {
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case HCL2_NULL:
    return true;
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

/* Builtins are resolved from a static table (see end of file), so they are
 * available even with a NULL context; a context's own functions take priority. */
static hcl2_func builtin_func(const char *name);

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
static const hcl2_value *ctx_var(hcl2_ctx *c, const char *name) {
  if (c == NULL)
    return NULL;
  for (size_t i = 0; i < c->nv; i++)
    if (strcmp(c->vars[i].name, name) == 0)
      return c->vars[i].val;
  return NULL;
}
static hcl2_func ctx_func(hcl2_ctx *c, const char *name) {
  if (c == NULL)
    return NULL;
  for (size_t i = 0; i < c->nf; i++)
    if (strcmp(c->fns[i].name, name) == 0)
      return c->fns[i].fn;
  return NULL;
}

/* ===========================================================================
 * Lexer
 * ===========================================================================*/
enum tok {
  T_EOF,
  T_ERR,
  T_NUM,
  T_STR,
  T_IDENT,
  T_LP,
  T_RP,
  T_LB,
  T_RB,
  T_LC,
  T_RC,
  T_COMMA,
  T_DOT,
  T_COLON,
  T_QUEST,
  T_ASSIGN,
  T_PLUS,
  T_MINUS,
  T_STAR,
  T_SLASH,
  T_PCT,
  T_EQ,
  T_NE,
  T_LT,
  T_LE,
  T_GT,
  T_GE,
  T_AND,
  T_OR,
  T_NOT,
};

struct lexer {
  const char *p, *end;
  enum tok tok;
  char *text; /* T_NUM/T_IDENT/T_STR (raw inner for strings); owned, reused */
  size_t tlen;
  char *err;
  size_t errsz;
};

static void lx_err(struct lexer *l, const char *m) {
  if (l->err && l->errsz && l->err[0] == '\0')
    snprintf(l->err, l->errsz, "hcl2: %s", m);
}
static bool settext(struct lexer *l, const char *s, size_t n) {
  char *t = realloc(l->text, n + 1);
  if (t == NULL)
    return false;
  l->text = t;
  memcpy(t, s, n);
  t[n] = '\0';
  l->tlen = n;
  return true;
}
static bool id_start(int c) { return isalpha(c) || c == '_'; }
static bool id_char(int c) { return isalnum(c) || c == '_' || c == '-'; }

static void lex(struct lexer *l) {
  while (l->p < l->end && isspace((unsigned char)*l->p))
    l->p++;
  if (l->p >= l->end) {
    l->tok = T_EOF;
    return;
  }
  char c = *l->p;
  switch (c) {
  case '(':
    l->p++;
    l->tok = T_LP;
    return;
  case ')':
    l->p++;
    l->tok = T_RP;
    return;
  case '[':
    l->p++;
    l->tok = T_LB;
    return;
  case ']':
    l->p++;
    l->tok = T_RB;
    return;
  case '{':
    l->p++;
    l->tok = T_LC;
    return;
  case '}':
    l->p++;
    l->tok = T_RC;
    return;
  case ',':
    l->p++;
    l->tok = T_COMMA;
    return;
  case '.':
    l->p++;
    l->tok = T_DOT;
    return;
  case ':':
    l->p++;
    l->tok = T_COLON;
    return;
  case '?':
    l->p++;
    l->tok = T_QUEST;
    return;
  case '+':
    l->p++;
    l->tok = T_PLUS;
    return;
  case '-':
    l->p++;
    l->tok = T_MINUS;
    return;
  case '*':
    l->p++;
    l->tok = T_STAR;
    return;
  case '/':
    l->p++;
    l->tok = T_SLASH;
    return;
  case '%':
    l->p++;
    l->tok = T_PCT;
    return;
  case '=':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_EQ;
      return;
    }
    l->p++;
    l->tok = T_ASSIGN;
    return;
  case '!':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_NE;
      return;
    }
    l->p++;
    l->tok = T_NOT;
    return;
  case '<':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_LE;
      return;
    }
    l->p++;
    l->tok = T_LT;
    return;
  case '>':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_GE;
      return;
    }
    l->p++;
    l->tok = T_GT;
    return;
  case '&':
    if (l->end - l->p >= 2 && l->p[1] == '&') {
      l->p += 2;
      l->tok = T_AND;
      return;
    }
    l->tok = T_ERR;
    lx_err(l, "unexpected '&'");
    return;
  case '|':
    if (l->end - l->p >= 2 && l->p[1] == '|') {
      l->p += 2;
      l->tok = T_OR;
      return;
    }
    l->tok = T_ERR;
    lx_err(l, "unexpected '|'");
    return;
  }
  if (c == '"') {
    /* capture raw inner bytes (escapes kept raw), stop at unescaped quote */
    const char *start = ++l->p;
    while (l->p < l->end && *l->p != '"') {
      if (*l->p == '\\' && l->p + 1 < l->end)
        l->p++;
      l->p++;
    }
    if (l->p >= l->end) {
      l->tok = T_ERR;
      lx_err(l, "unterminated string");
      return;
    }
    if (!settext(l, start, (size_t)(l->p - start))) {
      l->tok = T_ERR;
      return;
    }
    l->p++; /* closing quote */
    l->tok = T_STR;
    return;
  }
  if (isdigit((unsigned char)c)) {
    const char *start = l->p;
    while (l->p < l->end && (isdigit((unsigned char)*l->p) || *l->p == '.' || *l->p == 'e' ||
                             *l->p == 'E' || *l->p == '+' || *l->p == '-')) {
      /* allow exponent sign only right after e/E */
      if ((*l->p == '+' || *l->p == '-') && !(l->p > start && (l->p[-1] == 'e' || l->p[-1] == 'E')))
        break;
      l->p++;
    }
    if (!settext(l, start, (size_t)(l->p - start))) {
      l->tok = T_ERR;
      return;
    }
    l->tok = T_NUM;
    return;
  }
  if (id_start((unsigned char)c)) {
    const char *start = l->p;
    while (l->p < l->end && id_char((unsigned char)*l->p))
      l->p++;
    if (!settext(l, start, (size_t)(l->p - start))) {
      l->tok = T_ERR;
      return;
    }
    l->tok = T_IDENT;
    return;
  }
  l->tok = T_ERR;
  lx_err(l, "invalid character");
}

/* ===========================================================================
 * AST
 * ===========================================================================*/
enum nkind {
  N_LIT,
  N_TEMPLATE,
  N_VAR,
  N_ATTR,
  N_INDEX,
  N_UNARY,
  N_BINARY,
  N_COND,
  N_TUPLE,
  N_OBJECT,
  N_CALL,
};
struct node {
  enum nkind kind;
  hcl2_value *lit;        /* N_LIT */
  char *str;              /* N_TEMPLATE raw / N_VAR name / N_ATTR name / N_CALL name */
  enum tok op;            /* N_UNARY / N_BINARY */
  struct node *a, *b, *c; /* children */
  struct node **items;    /* N_TUPLE / N_CALL args / N_OBJECT vals */
  char **keys;            /* N_OBJECT keys */
  size_t n;
};

static void node_free(struct node *x) {
  if (x == NULL)
    return;
  hcl2_value_free(x->lit);
  free(x->str);
  node_free(x->a);
  node_free(x->b);
  node_free(x->c);
  for (size_t i = 0; i < x->n; i++) {
    node_free(x->items[i]);
    if (x->keys)
      free(x->keys[i]);
  }
  free(x->items);
  free(x->keys);
  free(x);
}
static struct node *nnew(enum nkind k) {
  struct node *x = calloc(1, sizeof(*x));
  if (x)
    x->kind = k;
  return x;
}

/* ===========================================================================
 * Parser (Pratt)
 * ===========================================================================*/
struct parser {
  struct lexer lx;
};
#define PERR(p, m)                                                                                 \
  do {                                                                                             \
    lx_err(&(p)->lx, m);                                                                           \
    return NULL;                                                                                   \
  } while (0)

static struct node *parse_expr(struct parser *p);

static struct node *parse_primary(struct parser *p) {
  struct lexer *l = &p->lx;
  switch (l->tok) {
  case T_NUM: {
    struct node *x = nnew(N_LIT);
    if (!x)
      return NULL;
    x->lit = hcl2_number(strtod(l->text, NULL));
    lex(l);
    return x;
  }
  case T_STR: {
    struct node *x = nnew(N_TEMPLATE);
    if (!x)
      return NULL;
    x->str = strdup(l->text);
    if (!x->str) {
      node_free(x);
      return NULL;
    }
    lex(l);
    return x;
  }
  case T_IDENT: {
    if (strcmp(l->text, "true") == 0 || strcmp(l->text, "false") == 0) {
      struct node *x = nnew(N_LIT);
      if (!x)
        return NULL;
      x->lit = hcl2_bool(l->text[0] == 't');
      lex(l);
      return x;
    }
    if (strcmp(l->text, "null") == 0) {
      struct node *x = nnew(N_LIT);
      if (!x)
        return NULL;
      x->lit = hcl2_null();
      lex(l);
      return x;
    }
    char *name = strdup(l->text);
    if (!name)
      return NULL;
    lex(l);
    if (l->tok == T_LP) { /* function call */
      struct node *x = nnew(N_CALL);
      if (!x) {
        free(name);
        return NULL;
      }
      x->str = name;
      lex(l); /* consume '(' */
      while (l->tok != T_RP) {
        struct node *arg = parse_expr(p);
        if (arg == NULL) {
          node_free(x);
          return NULL;
        }
        struct node **ni = realloc(x->items, (x->n + 1) * sizeof(*ni));
        if (!ni) {
          node_free(arg);
          node_free(x);
          return NULL;
        }
        x->items = ni;
        x->items[x->n++] = arg;
        if (l->tok == T_COMMA) {
          lex(l);
          continue;
        }
        break;
      }
      if (l->tok != T_RP) {
        node_free(x);
        PERR(p, "expected ')' after arguments");
      }
      lex(l);
      return x;
    }
    struct node *x = nnew(N_VAR);
    if (!x) {
      free(name);
      return NULL;
    }
    x->str = name;
    return x;
  }
  case T_LP: {
    lex(l);
    struct node *e = parse_expr(p);
    if (e == NULL)
      return NULL;
    if (l->tok != T_RP) {
      node_free(e);
      PERR(p, "expected ')'");
    }
    lex(l);
    return e;
  }
  case T_LB: { /* tuple */
    struct node *x = nnew(N_TUPLE);
    if (!x)
      return NULL;
    lex(l);
    while (l->tok != T_RB) {
      struct node *e = parse_expr(p);
      if (e == NULL) {
        node_free(x);
        return NULL;
      }
      struct node **ni = realloc(x->items, (x->n + 1) * sizeof(*ni));
      if (!ni) {
        node_free(e);
        node_free(x);
        return NULL;
      }
      x->items = ni;
      x->items[x->n++] = e;
      if (l->tok == T_COMMA) {
        lex(l);
        continue;
      }
      break;
    }
    if (l->tok != T_RB) {
      node_free(x);
      PERR(p, "expected ']' in tuple");
    }
    lex(l);
    return x;
  }
  case T_LC: { /* object */
    struct node *x = nnew(N_OBJECT);
    if (!x)
      return NULL;
    lex(l);
    while (l->tok != T_RC) {
      if (l->tok != T_IDENT && l->tok != T_STR) {
        node_free(x);
        PERR(p, "expected an object key");
      }
      char *key = strdup(l->text);
      if (!key) {
        node_free(x);
        return NULL;
      }
      lex(l);
      if (l->tok != T_ASSIGN && l->tok != T_COLON) {
        free(key);
        node_free(x);
        PERR(p, "expected '=' after object key");
      }
      lex(l);
      struct node *val = parse_expr(p);
      if (val == NULL) {
        free(key);
        node_free(x);
        return NULL;
      }
      struct node **ni = realloc(x->items, (x->n + 1) * sizeof(*ni));
      char **nk = realloc(x->keys, (x->n + 1) * sizeof(*nk));
      if (!ni || !nk) {
        free(key);
        node_free(val);
        node_free(x);
        free(ni == NULL ? NULL : ni); /* best-effort */
        return NULL;
      }
      x->items = ni;
      x->keys = nk;
      x->keys[x->n] = key;
      x->items[x->n] = val;
      x->n++;
      if (l->tok == T_COMMA) {
        lex(l);
        continue;
      }
      /* allow newline-separated (already skipped as whitespace) */
    }
    lex(l);
    return x;
  }
  default:
    PERR(p, "expected an expression");
  }
}

static struct node *parse_postfix(struct parser *p) {
  struct node *e = parse_primary(p);
  if (e == NULL)
    return NULL;
  struct lexer *l = &p->lx;
  for (;;) {
    if (l->tok == T_DOT) {
      lex(l);
      if (l->tok != T_IDENT) {
        node_free(e);
        PERR(p, "expected attribute name after '.'");
      }
      struct node *x = nnew(N_ATTR);
      if (!x) {
        node_free(e);
        return NULL;
      }
      x->a = e;
      x->str = strdup(l->text);
      if (!x->str) {
        node_free(x);
        return NULL;
      }
      lex(l);
      e = x;
    } else if (l->tok == T_LB) {
      lex(l);
      struct node *idx = parse_expr(p);
      if (idx == NULL) {
        node_free(e);
        return NULL;
      }
      if (l->tok != T_RB) {
        node_free(e);
        node_free(idx);
        PERR(p, "expected ']' after index");
      }
      lex(l);
      struct node *x = nnew(N_INDEX);
      if (!x) {
        node_free(e);
        node_free(idx);
        return NULL;
      }
      x->a = e;
      x->b = idx;
      e = x;
    } else {
      break;
    }
  }
  return e;
}

static struct node *parse_unary(struct parser *p) {
  struct lexer *l = &p->lx;
  if (l->tok == T_MINUS || l->tok == T_NOT) {
    enum tok op = l->tok;
    lex(l);
    struct node *e = parse_unary(p);
    if (e == NULL)
      return NULL;
    struct node *x = nnew(N_UNARY);
    if (!x) {
      node_free(e);
      return NULL;
    }
    x->op = op;
    x->a = e;
    return x;
  }
  return parse_postfix(p);
}

static int binbp(enum tok t) {
  switch (t) {
  case T_OR:
    return 1;
  case T_AND:
    return 2;
  case T_EQ:
  case T_NE:
    return 3;
  case T_LT:
  case T_LE:
  case T_GT:
  case T_GE:
    return 4;
  case T_PLUS:
  case T_MINUS:
    return 5;
  case T_STAR:
  case T_SLASH:
  case T_PCT:
    return 6;
  default:
    return 0;
  }
}

static struct node *parse_binary(struct parser *p, int minbp) {
  struct node *left = parse_unary(p);
  if (left == NULL)
    return NULL;
  struct lexer *l = &p->lx;
  for (;;) {
    int bp = binbp(l->tok);
    if (bp < minbp || bp == 0)
      break;
    enum tok op = l->tok;
    lex(l);
    struct node *right = parse_binary(p, bp + 1);
    if (right == NULL) {
      node_free(left);
      return NULL;
    }
    struct node *x = nnew(N_BINARY);
    if (!x) {
      node_free(left);
      node_free(right);
      return NULL;
    }
    x->op = op;
    x->a = left;
    x->b = right;
    left = x;
  }
  return left;
}

static struct node *parse_expr(struct parser *p) {
  struct node *e = parse_binary(p, 1);
  if (e == NULL)
    return NULL;
  struct lexer *l = &p->lx;
  if (l->tok == T_QUEST) {
    lex(l);
    struct node *a = parse_expr(p);
    if (a == NULL) {
      node_free(e);
      return NULL;
    }
    if (l->tok != T_COLON) {
      node_free(e);
      node_free(a);
      PERR(p, "expected ':' in conditional");
    }
    lex(l);
    struct node *b = parse_expr(p);
    if (b == NULL) {
      node_free(e);
      node_free(a);
      return NULL;
    }
    struct node *x = nnew(N_COND);
    if (!x) {
      node_free(e);
      node_free(a);
      node_free(b);
      return NULL;
    }
    x->a = e;
    x->b = a;
    x->c = b;
    return x;
  }
  return e;
}

/* ===========================================================================
 * Evaluator
 * ===========================================================================*/
struct sbuf {
  char *p;
  size_t len, cap;
  bool oom;
};
static void sb_putn(struct sbuf *s, const char *d, size_t n) {
  if (s->oom)
    return;
  if (s->len + n + 1 > s->cap) {
    size_t cap = s->cap ? s->cap * 2 : 64;
    while (cap < s->len + n + 1)
      cap *= 2;
    char *np = realloc(s->p, cap);
    if (!np) {
      s->oom = true;
      return;
    }
    s->p = np;
    s->cap = cap;
  }
  memcpy(s->p + s->len, d, n);
  s->len += n;
  s->p[s->len] = '\0';
}
static void sb_putc(struct sbuf *s, char c) { sb_putn(s, &c, 1); }
static void sb_puts(struct sbuf *s, const char *str) { sb_putn(s, str, strlen(str)); }

static void everr(char *err, size_t errsz, const char *m) {
  if (err && errsz && err[0] == '\0')
    snprintf(err, errsz, "hcl2: %s", m);
}

/* Append a scalar value's textual form for template interpolation. */
static bool val_to_text(const hcl2_value *v, struct sbuf *s, char *err, size_t errsz) {
  char buf[40];
  switch (v->kind) {
  case HCL2_STRING:
    sb_puts(s, v->str);
    return true;
  case HCL2_NUMBER:
    snprintf(buf, sizeof(buf), "%g", v->num);
    sb_puts(s, buf);
    return true;
  case HCL2_BOOL:
    sb_puts(s, v->b ? "true" : "false");
    return true;
  default:
    everr(err, errsz, "cannot interpolate a null/tuple/object into a string");
    return false;
  }
}

static hcl2_value *eval(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz);

/* Evaluate a string template (raw inner bytes). */
static hcl2_value *eval_template(const char *raw, hcl2_ctx *ctx, char *err, size_t errsz) {
  struct sbuf s = {0};
  const char *p = raw, *end = raw + strlen(raw);
  while (p < end) {
    if (p[0] == '$' && p + 1 < end && p[1] == '{') {
      /* find matching '}' with brace depth */
      const char *s0 = p + 2;
      const char *q = s0;
      int depth = 1;
      while (q < end && depth > 0) {
        if (*q == '{')
          depth++;
        else if (*q == '}')
          depth--;
        if (depth == 0)
          break;
        q++;
      }
      if (depth != 0) {
        everr(err, errsz, "unterminated ${ ... } in template");
        free(s.p);
        return NULL;
      }
      hcl2_value *iv = hcl2_eval(s0, (size_t)(q - s0), ctx, err, errsz);
      if (iv == NULL) {
        free(s.p);
        return NULL;
      }
      bool ok = val_to_text(iv, &s, err, errsz);
      hcl2_value_free(iv);
      if (!ok) {
        free(s.p);
        return NULL;
      }
      p = q + 1;
      continue;
    }
    if (p[0] == '$' && p + 2 < end && p[1] == '$' && p[2] == '{') {
      sb_puts(&s, "${");
      p += 3;
      continue;
    }
    if (p[0] == '%' && p + 1 < end && p[1] == '{') {
      everr(err, errsz, "template directives %{ ... } are not supported yet");
      free(s.p);
      return NULL;
    }
    if (p[0] == '\\' && p + 1 < end) {
      char e = p[1];
      char ch = e;
      if (e == 'n')
        ch = '\n';
      else if (e == 't')
        ch = '\t';
      else if (e == 'r')
        ch = '\r';
      sb_putc(&s, ch);
      p += 2;
      continue;
    }
    sb_putc(&s, *p);
    p++;
  }
  if (s.oom) {
    free(s.p);
    return NULL;
  }
  hcl2_value *v = hcl2_string(s.p ? s.p : "");
  free(s.p);
  return v;
}

static hcl2_value *eval_binary(enum tok op, hcl2_value *l, hcl2_value *r, char *err, size_t errsz) {
  hcl2_value *res = NULL;
  if (op == T_EQ || op == T_NE) {
    bool eq = vequal(l, r);
    res = hcl2_bool(op == T_EQ ? eq : !eq);
    goto done;
  }
  if (op == T_AND || op == T_OR) {
    if (l->kind != HCL2_BOOL || r->kind != HCL2_BOOL) {
      everr(err, errsz, "logical operators require booleans");
      goto done;
    }
    res = hcl2_bool(op == T_AND ? (l->b && r->b) : (l->b || r->b));
    goto done;
  }
  /* arithmetic + comparison: numbers */
  if (l->kind != HCL2_NUMBER || r->kind != HCL2_NUMBER) {
    everr(err, errsz, "arithmetic/comparison requires numbers");
    goto done;
  }
  double a = l->num, b = r->num;
  switch (op) {
  case T_PLUS:
    res = hcl2_number(a + b);
    break;
  case T_MINUS:
    res = hcl2_number(a - b);
    break;
  case T_STAR:
    res = hcl2_number(a * b);
    break;
  case T_SLASH:
    if (b == 0) {
      everr(err, errsz, "division by zero");
      break;
    }
    res = hcl2_number(a / b);
    break;
  case T_PCT:
    if (b == 0) {
      everr(err, errsz, "modulo by zero");
      break;
    }
    res = hcl2_number(fmod(a, b));
    break;
  case T_LT:
    res = hcl2_bool(a < b);
    break;
  case T_LE:
    res = hcl2_bool(a <= b);
    break;
  case T_GT:
    res = hcl2_bool(a > b);
    break;
  case T_GE:
    res = hcl2_bool(a >= b);
    break;
  default:
    everr(err, errsz, "unknown operator");
    break;
  }
done:
  hcl2_value_free(l);
  hcl2_value_free(r);
  return res;
}

static hcl2_value *eval(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz) {
  switch (x->kind) {
  case N_LIT:
    return vclone(x->lit);
  case N_TEMPLATE:
    return eval_template(x->str, ctx, err, errsz);
  case N_VAR: {
    const hcl2_value *v = ctx_var(ctx, x->str);
    if (v == NULL) {
      char m[160];
      snprintf(m, sizeof(m), "undefined variable \"%s\"", x->str);
      everr(err, errsz, m);
      return NULL;
    }
    return vclone(v);
  }
  case N_ATTR: {
    hcl2_value *o = eval(x->a, ctx, err, errsz);
    if (o == NULL)
      return NULL;
    const hcl2_value *f = hcl2_value_get(o, x->str);
    if (f == NULL) {
      char m[160];
      snprintf(m, sizeof(m), "no attribute \"%s\"", x->str);
      everr(err, errsz, m);
      hcl2_value_free(o);
      return NULL;
    }
    hcl2_value *res = vclone(f);
    hcl2_value_free(o);
    return res;
  }
  case N_INDEX: {
    hcl2_value *base = eval(x->a, ctx, err, errsz);
    if (base == NULL)
      return NULL;
    hcl2_value *idx = eval(x->b, ctx, err, errsz);
    if (idx == NULL) {
      hcl2_value_free(base);
      return NULL;
    }
    const hcl2_value *f = NULL;
    if (base->kind == HCL2_TUPLE && idx->kind == HCL2_NUMBER) {
      double d = idx->num;
      if (d >= 0 && d < (double)base->n)
        f = base->items[(size_t)d];
    } else if (base->kind == HCL2_OBJECT && idx->kind == HCL2_STRING) {
      f = hcl2_value_get(base, idx->str);
    }
    if (f == NULL)
      everr(err, errsz, "index out of range or wrong key/type");
    hcl2_value *res = f ? vclone(f) : NULL;
    hcl2_value_free(base);
    hcl2_value_free(idx);
    return res;
  }
  case N_UNARY: {
    hcl2_value *e = eval(x->a, ctx, err, errsz);
    if (e == NULL)
      return NULL;
    hcl2_value *res = NULL;
    if (x->op == T_MINUS && e->kind == HCL2_NUMBER)
      res = hcl2_number(-e->num);
    else if (x->op == T_NOT && e->kind == HCL2_BOOL)
      res = hcl2_bool(!e->b);
    else
      everr(err, errsz, "unary operator type mismatch");
    hcl2_value_free(e);
    return res;
  }
  case N_BINARY: {
    hcl2_value *l = eval(x->a, ctx, err, errsz);
    if (l == NULL)
      return NULL;
    hcl2_value *r = eval(x->b, ctx, err, errsz);
    if (r == NULL) {
      hcl2_value_free(l);
      return NULL;
    }
    return eval_binary(x->op, l, r, err, errsz);
  }
  case N_COND: {
    hcl2_value *c = eval(x->a, ctx, err, errsz);
    if (c == NULL)
      return NULL;
    if (c->kind != HCL2_BOOL) {
      everr(err, errsz, "condition must be a boolean");
      hcl2_value_free(c);
      return NULL;
    }
    bool t = c->b;
    hcl2_value_free(c);
    return eval(t ? x->b : x->c, ctx, err, errsz);
  }
  case N_TUPLE: {
    hcl2_value *t = hcl2_tuple();
    if (!t)
      return NULL;
    for (size_t i = 0; i < x->n; i++) {
      hcl2_value *e = eval(x->items[i], ctx, err, errsz);
      if (e == NULL || !hcl2_tuple_push(t, e)) {
        hcl2_value_free(e);
        hcl2_value_free(t);
        return NULL;
      }
    }
    return t;
  }
  case N_OBJECT: {
    hcl2_value *o = hcl2_object();
    if (!o)
      return NULL;
    for (size_t i = 0; i < x->n; i++) {
      hcl2_value *v = eval(x->items[i], ctx, err, errsz);
      if (v == NULL || !hcl2_object_set(o, x->keys[i], v)) {
        hcl2_value_free(v);
        hcl2_value_free(o);
        return NULL;
      }
    }
    return o;
  }
  case N_CALL: {
    hcl2_func fn = ctx_func(ctx, x->str);
    if (fn == NULL)
      fn = builtin_func(x->str);
    if (fn == NULL) {
      char m[160];
      snprintf(m, sizeof(m), "unknown function \"%s\"", x->str);
      everr(err, errsz, m);
      return NULL;
    }
    hcl2_value **args = x->n ? calloc(x->n, sizeof(*args)) : NULL;
    if (x->n && args == NULL)
      return NULL;
    bool ok = true;
    size_t i = 0;
    for (; i < x->n; i++) {
      args[i] = eval(x->items[i], ctx, err, errsz);
      if (args[i] == NULL) {
        ok = false;
        break;
      }
    }
    hcl2_value *res = NULL;
    if (ok)
      res = fn((const hcl2_value *const *)args, x->n, err, errsz);
    for (size_t j = 0; j < i; j++)
      hcl2_value_free(args[j]);
    free(args);
    return res;
  }
  }
  return NULL;
}

hcl2_value *hcl2_eval(const char *src, size_t len, hcl2_ctx *ctx, char *err, size_t errsz) {
  if (err && errsz)
    err[0] = '\0';
  struct parser p = {0};
  p.lx.p = src;
  p.lx.end = src + len;
  p.lx.err = err;
  p.lx.errsz = errsz;
  lex(&p.lx);
  struct node *root = parse_expr(&p);
  if (root == NULL) {
    free(p.lx.text);
    everr(err, errsz, "parse error");
    return NULL;
  }
  if (p.lx.tok != T_EOF) {
    node_free(root);
    free(p.lx.text);
    everr(err, errsz, "trailing tokens after expression");
    return NULL;
  }
  hcl2_value *v = eval(root, ctx, err, errsz);
  node_free(root);
  free(p.lx.text);
  return v;
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

static hcl2_func builtin_func(const char *name) {
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

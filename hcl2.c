#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_internal.h"

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

void everr(char *err, size_t errsz, const char *m) {
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

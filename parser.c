#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * AST
 * ===========================================================================*/
/* enum nkind / struct node live in hcl2_internal.h. */

void node_free(struct node *x) {
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
 * Parser (Pratt) -- struct parser lives in hcl2_internal.h
 * ===========================================================================*/
#define PERR(p, m)                                                                                 \
  do {                                                                                             \
    lx_err(&(p)->lx, m);                                                                           \
    return NULL;                                                                                   \
  } while (0)

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

struct node *parse_expr(struct parser *p) {
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

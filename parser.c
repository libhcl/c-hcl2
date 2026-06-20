#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
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
  free(x->kvar);
  node_free(x->a);
  node_free(x->b);
  node_free(x->c);
  node_free(x->d);
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
/* Record the source position `pos` on a node, for eval-error diagnostics. */
static void stamp(struct parser *p, struct node *x, const char *pos) {
  if (x != NULL)
    lx_linecol(&p->lx, pos, &x->line, &x->col);
}
/* Record the node's end span: the position just past its last consumed token
 * (the lexer's prevend after that token was lexed). */
static void stamp_end(struct parser *p, struct node *x) {
  if (x != NULL)
    lx_linecol(&p->lx, p->lx.prevend, &x->endline, &x->endcol);
}
/* Inherit the end span of a (rightmost) child node. */
static void end_copy(struct node *x, const struct node *from) {
  if (x != NULL && from != NULL) {
    x->endline = from->endline;
    x->endcol = from->endcol;
  }
}

/* ===========================================================================
 * Parser (Pratt) -- struct parser lives in hcl2_internal.h
 * ===========================================================================*/
#define PERR(p, m)                                                                                 \
  do {                                                                                             \
    lx_err(&(p)->lx, m);                                                                           \
    return NULL;                                                                                   \
  } while (0)

static bool kw(struct lexer *l, const char *w) {
  return l->tok == T_IDENT && strcmp(l->text, w) == 0;
}

/* Parse a for-expression. The current token is the `for` keyword; the opening
 * '[' or '{' has already been consumed by the caller. `object` selects the
 * `{for k => v}` form over the `[for v]` tuple form. */
static struct node *parse_for(struct parser *p, bool object) {
  struct lexer *l = &p->lx;
  const char *forpos = l->tokpos; /* the 'for' keyword, for the span start */
  lex(l);                         /* consume 'for' */
  if (l->tok != T_IDENT)
    PERR(p, "expected a variable name after 'for'");
  char *v1 = strdup(l->text);
  if (v1 == NULL)
    return NULL;
  lex(l);
  char *v2 = NULL;
  if (l->tok == T_COMMA) {
    lex(l);
    if (l->tok != T_IDENT) {
      free(v1);
      PERR(p, "expected a second variable name after ','");
    }
    v2 = strdup(l->text);
    if (v2 == NULL) {
      free(v1);
      return NULL;
    }
    lex(l);
  }
  if (!kw(l, "in")) {
    free(v1);
    free(v2);
    PERR(p, "expected 'in' in for-expression");
  }
  lex(l); /* consume 'in' */
  struct node *coll = parse_expr(p);
  if (coll == NULL) {
    free(v1);
    free(v2);
    return NULL;
  }
  if (l->tok != T_COLON) {
    free(v1);
    free(v2);
    node_free(coll);
    PERR(p, "expected ':' in for-expression");
  }
  lex(l); /* consume ':' */
  struct node *f = nnew(object ? N_FOR_OBJECT : N_FOR_TUPLE);
  if (f == NULL) {
    free(v1);
    free(v2);
    node_free(coll);
    return NULL;
  }
  f->a = coll;
  stamp(p, f, forpos);
  /* one var -> value var; two vars -> key var, value var */
  if (v2 != NULL) {
    f->kvar = v1;
    f->str = v2;
  } else {
    f->str = v1;
  }
  f->b = parse_expr(p); /* tuple: result expr; object: key expr */
  if (f->b == NULL) {
    node_free(f);
    return NULL;
  }
  if (object) {
    if (l->tok != T_FATARROW) {
      node_free(f);
      PERR(p, "expected '=>' in object for-expression");
    }
    lex(l);
    f->c = parse_expr(p); /* value expr */
    if (f->c == NULL) {
      node_free(f);
      return NULL;
    }
    if (l->tok == T_ELLIPSIS) { /* grouping mode: key -> tuple of values */
      f->op = T_ELLIPSIS;
      lex(l);
    }
  }
  if (kw(l, "if")) { /* optional filter */
    lex(l);
    f->d = parse_expr(p);
    if (f->d == NULL) {
      node_free(f);
      return NULL;
    }
  }
  enum tok close = object ? T_RC : T_RB;
  if (l->tok != close) {
    node_free(f);
    PERR(p, object ? "expected '}' to close object for-expression"
                   : "expected ']' to close for-expression");
  }
  lex(l); /* consume closing bracket */
  stamp_end(p, f);
  return f;
}

static struct node *parse_primary(struct parser *p) {
  struct lexer *l = &p->lx;
  switch (l->tok) {
  case T_NUM: {
    const char *pos = l->tokpos;
    struct node *x = nnew(N_LIT);
    if (!x)
      return NULL;
    x->lit = hcl2_number(strtod(l->text, NULL));
    stamp(p, x, pos);
    lex(l);
    stamp_end(p, x);
    return x;
  }
  case T_STR:
  case T_HEREDOC: {
    const char *pos = l->tokpos;
    struct node *x = nnew(N_TEMPLATE);
    if (!x)
      return NULL;
    x->op = l->tok; /* T_HEREDOC marks a raw template (no backslash escapes) */
    x->str = strdup(l->text);
    if (!x->str) {
      node_free(x);
      return NULL;
    }
    stamp(p, x, pos);
    lex(l);
    stamp_end(p, x);
    return x;
  }
  case T_IDENT: {
    if (strcmp(l->text, "true") == 0 || strcmp(l->text, "false") == 0) {
      const char *pos = l->tokpos;
      struct node *x = nnew(N_LIT);
      if (!x)
        return NULL;
      x->lit = hcl2_bool(l->text[0] == 't');
      stamp(p, x, pos);
      lex(l);
      stamp_end(p, x);
      return x;
    }
    if (strcmp(l->text, "null") == 0) {
      const char *pos = l->tokpos;
      struct node *x = nnew(N_LIT);
      if (!x)
        return NULL;
      x->lit = hcl2_null();
      stamp(p, x, pos);
      lex(l);
      stamp_end(p, x);
      return x;
    }
    const char *idpos = l->tokpos; /* position of the identifier */
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
      stamp(p, x, idpos);
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
        if (l->tok == T_ELLIPSIS) { /* spread: must be the final argument */
          x->op = T_ELLIPSIS;
          lex(l);
          break;
        }
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
      stamp_end(p, x);
      return x;
    }
    struct node *x = nnew(N_VAR);
    if (!x) {
      free(name);
      return NULL;
    }
    x->str = name;
    stamp(p, x, idpos);
    stamp_end(p, x);
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
  case T_LB: { /* tuple or [for ...] */
    const char *pos = l->tokpos;
    lex(l);
    if (kw(l, "for"))
      return parse_for(p, false);
    struct node *x = nnew(N_TUPLE);
    if (!x)
      return NULL;
    stamp(p, x, pos);
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
    stamp_end(p, x);
    return x;
  }
  case T_LC: { /* object or {for ...} */
    const char *pos = l->tokpos;
    lex(l);
    if (kw(l, "for"))
      return parse_for(p, true);
    struct node *x = nnew(N_OBJECT);
    if (!x)
      return NULL;
    stamp(p, x, pos);
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
      /* Grow each array and assign back immediately, so x always owns valid
         buffers -- otherwise a failed second realloc would leave x->items
         dangling for node_free(x). */
      struct node **ni = realloc(x->items, (x->n + 1) * sizeof(*ni));
      if (!ni) {
        free(key);
        node_free(val);
        node_free(x);
        return NULL;
      }
      x->items = ni;
      char **nk = realloc(x->keys, (x->n + 1) * sizeof(*nk));
      if (!nk) {
        free(key);
        node_free(val);
        node_free(x);
        return NULL;
      }
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
    stamp_end(p, x);
    return x;
  }
  default:
    PERR(p, "expected an expression");
  }
}

/* Build a splat from a collection node `coll` (ownership transferred): the
 * `[*]` or `.*` marker has been consumed; this consumes the following relative
 * traversal (a chain of `.attr` and `[index]`) and desugars the whole thing to
 * `[for $splat in coll : $splat <traversal>]`. A chained splat (`[*][*]`,
 * `.*.* `) is rejected. Returns NULL on error, freeing `coll`. */
static struct node *build_splat(struct parser *p, struct node *coll) {
  struct lexer *l = &p->lx;
  struct node *body = nnew(N_VAR);
  if (body == NULL) {
    node_free(coll);
    return NULL;
  }
  body->str = strdup("$splat");
  if (body->str == NULL) {
    node_free(body);
    node_free(coll);
    return NULL;
  }
  for (;;) {
    if (l->tok == T_DOT) {
      lex(l);
      if (l->tok == T_STAR) { /* chained attribute splat: .*.* */
        lex(l);
        body = build_splat(p, body); /* takes ownership of body, frees on error */
        if (body == NULL) {
          node_free(coll);
          return NULL;
        }
        continue;
      }
      if (l->tok != T_IDENT) {
        lx_err(l, "expected attribute name after '.' in splat");
        node_free(body);
        node_free(coll);
        return NULL;
      }
      struct node *at = nnew(N_ATTR);
      if (at == NULL) {
        node_free(body);
        node_free(coll);
        return NULL;
      }
      at->a = body;
      body = at;
      at->str = strdup(l->text);
      if (at->str == NULL) {
        node_free(body);
        node_free(coll);
        return NULL;
      }
      lex(l);
    } else if (l->tok == T_LB) {
      lex(l);
      if (l->tok == T_STAR) { /* chained full splat: [*][*] */
        lex(l);
        if (l->tok != T_RB) {
          lx_err(l, "expected ']' after '[*]'");
          node_free(body);
          node_free(coll);
          return NULL;
        }
        lex(l);
        body = build_splat(p, body); /* takes ownership of body, frees on error */
        if (body == NULL) {
          node_free(coll);
          return NULL;
        }
        continue;
      }
      struct node *idx = parse_expr(p);
      if (idx == NULL) {
        node_free(body);
        node_free(coll);
        return NULL;
      }
      if (l->tok != T_RB) {
        lx_err(l, "expected ']' after index");
        node_free(idx);
        node_free(body);
        node_free(coll);
        return NULL;
      }
      lex(l);
      struct node *ix = nnew(N_INDEX);
      if (ix == NULL) {
        node_free(idx);
        node_free(body);
        node_free(coll);
        return NULL;
      }
      ix->a = body;
      ix->b = idx;
      body = ix;
    } else {
      break;
    }
  }
  struct node *f = nnew(N_FOR_TUPLE);
  if (f == NULL) {
    node_free(body);
    node_free(coll);
    return NULL;
  }
  f->a = coll;
  f->b = body;
  f->str = strdup("$splat");
  if (f->str == NULL) {
    node_free(f);
    return NULL;
  }
  end_copy(f, coll); /* fallback start/end from the collection */
  f->line = coll->line;
  f->col = coll->col;
  stamp_end(p, f);
  return f;
}

static struct node *parse_postfix(struct parser *p) {
  struct node *e = parse_primary(p);
  if (e == NULL)
    return NULL;
  struct lexer *l = &p->lx;
  for (;;) {
    if (l->tok == T_DOT) {
      const char *dotpos = l->tokpos;
      lex(l);
      if (l->tok == T_STAR) { /* legacy attribute splat: e.*.attr */
        lex(l);
        e = build_splat(p, e);
        if (e == NULL)
          return NULL;
        continue;
      }
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
      stamp(p, x, dotpos);
      lex(l);
      stamp_end(p, x);
      e = x;
    } else if (l->tok == T_LB) {
      const char *lbpos = l->tokpos;
      lex(l);
      if (l->tok == T_STAR) { /* full splat: e[*]<traversal> */
        lex(l);
        if (l->tok != T_RB) {
          node_free(e);
          PERR(p, "expected ']' after '[*]'");
        }
        lex(l);
        e = build_splat(p, e);
        if (e == NULL)
          return NULL;
        continue;
      }
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
      stamp(p, x, lbpos);
      stamp_end(p, x);
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
    const char *oppos = l->tokpos;
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
    stamp(p, x, oppos);
    end_copy(x, e);
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
    const char *oppos = l->tokpos;
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
    stamp(p, x, oppos);
    end_copy(x, right);
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
    x->line = e->line;
    x->col = e->col;
    end_copy(x, b);
    return x;
  }
  return e;
}

bool hcl2_expr_span(const char *src, size_t len, int *start_line, int *start_col, int *end_line,
                    int *end_col, char *err, size_t errsz) {
  if (err && errsz)
    err[0] = '\0';
  struct parser p = {0};
  p.lx.p = src;
  p.lx.end = src + len;
  p.lx.start = src;
  p.lx.tokpos = src;
  p.lx.err = err;
  p.lx.errsz = errsz;
  lex(&p.lx);
  struct node *root = parse_expr(&p);
  if (root == NULL) {
    free(p.lx.text);
    everr(err, errsz, "parse error");
    return false;
  }
  if (p.lx.tok != T_EOF) {
    node_free(root);
    free(p.lx.text);
    everr(err, errsz, "trailing tokens after expression");
    return false;
  }
  if (start_line)
    *start_line = root->line;
  if (start_col)
    *start_col = root->col;
  if (end_line)
    *end_line = root->endline;
  if (end_col)
    *end_col = root->endcol;
  node_free(root);
  free(p.lx.text);
  return true;
}

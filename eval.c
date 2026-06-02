#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_internal.h"

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

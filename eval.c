#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
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
/* Append a Unicode code point as UTF-8 (used by \uNNNN / \UNNNNNNNN escapes). */
static void sb_utf8(struct sbuf *s, uint32_t cp) {
  char b[4];
  if (cp < 0x80) {
    b[0] = (char)cp;
    sb_putn(s, b, 1);
  } else if (cp < 0x800) {
    b[0] = (char)(0xC0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3F));
    sb_putn(s, b, 2);
  } else if (cp < 0x10000) {
    b[0] = (char)(0xE0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    sb_putn(s, b, 3);
  } else {
    b[0] = (char)(0xF0 | (cp >> 18));
    b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (char)(0x80 | (cp & 0x3F));
    sb_putn(s, b, 4);
  }
}
static int hexval(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
/* Strip the trailing run of whitespace already emitted (for `${~`/`%{~`). */
static void sb_rtrim(struct sbuf *s) {
  while (s->len > 0 && isspace((unsigned char)s->p[s->len - 1]))
    s->len--;
  if (s->p != NULL)
    s->p[s->len] = '\0';
}

void everr(char *err, size_t errsz, const char *m) {
  if (err && errsz && err[0] == '\0')
    snprintf(err, errsz, "hcl2: %s", m);
}
/* everr with a source position (from a stamped AST node); line == 0 omits it. */
static void everr_at(char *err, size_t errsz, const char *m, int line, int col) {
  if (!(err && errsz && err[0] == '\0'))
    return;
  if (line > 0)
    snprintf(err, errsz, "hcl2: %s at line %d, column %d", m, line, col);
  else
    snprintf(err, errsz, "hcl2: %s", m);
}
static void everr_node(char *err, size_t errsz, const char *m, const struct node *x) {
  everr_at(err, errsz, m, x ? x->line : 0, x ? x->col : 0);
}
static bool is_unknown(const hcl2_value *v) { return v != NULL && v->kind == HCL2_UNKNOWN; }

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

hcl2_value *hcl2_eval_node(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz);

/* ---------------------------------------------------------------------------
 * Template engine: text with `${ expr }` interpolation, `$${` / `%%{` escapes,
 * and `%{ if }` / `%{ for }` control directives (M3). It is a small recursive
 * renderer; `active` lets an inactive branch (a false `if`, an empty `for`) be
 * traversed structurally without emitting output or evaluating expressions.
 * ------------------------------------------------------------------------- */
enum dirstop { STOP_END, STOP_ELSE, STOP_ENDIF, STOP_ENDFOR };

struct trender {
  const char *p, *end;
  bool heredoc; /* keep backslashes literal */
  bool active;  /* emit + evaluate, or traverse structurally */
  bool unknown; /* an interpolated/conditional unknown makes the whole result unknown */
  hcl2_ctx *ctx;
  char *err;
  size_t errsz;
  bool fail;
};

/* From s0 (just past an opening '{'), return the matching '}' or NULL. */
static const char *brace_match(const char *s0, const char *end) {
  const char *q = s0;
  int depth = 1;
  while (q < end) {
    if (*q == '{')
      depth++;
    else if (*q == '}' && --depth == 0)
      return q;
    q++;
  }
  return NULL;
}

/* Skip the input whitespace run (for the `~}` strip marker). */
static void t_rtrim(struct trender *t) {
  while (t->p < t->end && isspace((unsigned char)*t->p))
    t->p++;
}

/* Advance past the next '}' (closing a directive's `%{ ... }`), honouring a
 * trailing `~` (strip following whitespace). */
static void skip_to_brace(struct trender *t) {
  const char *close = brace_match(t->p, t->end);
  if (close == NULL) {
    everr(t->err, t->errsz, "unterminated %{ } directive");
    t->fail = true;
    return;
  }
  bool rt = (close > t->p && close[-1] == '~');
  t->p = close + 1;
  if (rt)
    t_rtrim(t);
}

/* Read an identifier (skipping leading spaces); returns a malloc'd copy or NULL. */
static char *dir_ident(struct trender *t) {
  while (t->p < t->end && isspace((unsigned char)*t->p))
    t->p++;
  const char *s0 = t->p;
  if (t->p < t->end && (isalpha((unsigned char)*t->p) || *t->p == '_')) {
    t->p++;
    while (t->p < t->end && (isalnum((unsigned char)*t->p) || *t->p == '_' || *t->p == '-'))
      t->p++;
  }
  size_t n = (size_t)(t->p - s0);
  if (n == 0)
    return NULL;
  char *id = malloc(n + 1);
  if (id == NULL)
    return NULL;
  memcpy(id, s0, n);
  id[n] = '\0';
  return id;
}

static void trender(struct trender *t, struct sbuf *s, enum dirstop *stop);

static void handle_if(struct trender *t, struct sbuf *s) {
  const char *close = brace_match(t->p, t->end); /* the directive's own '{' */
  if (close == NULL) {
    everr(t->err, t->errsz, "unterminated %{ if } directive");
    t->fail = true;
    return;
  }
  const char *cend = close;
  bool rt = (cend > t->p && cend[-1] == '~'); /* `~}` strips following whitespace */
  if (rt)
    cend--;
  bool cond = false, cunk = false;
  if (t->active) {
    hcl2_value *c = hcl2_eval(t->p, (size_t)(cend - t->p), t->ctx, t->err, t->errsz);
    if (c == NULL) {
      t->fail = true;
      return;
    }
    if (c->kind == HCL2_UNKNOWN) {
      cunk = true; /* unknown condition: render no branch, result is unknown */
      t->unknown = true;
    } else if (c->kind != HCL2_BOOL) {
      everr(t->err, t->errsz, "%{ if } condition must be a boolean");
      hcl2_value_free(c);
      t->fail = true;
      return;
    } else {
      cond = c->b;
    }
    hcl2_value_free(c);
  }
  t->p = close + 1;
  if (rt)
    t_rtrim(t);
  bool save = t->active;
  enum dirstop stop;
  t->active = save && cond && !cunk;
  trender(t, s, &stop);
  if (t->fail)
    return;
  if (stop == STOP_ELSE) {
    t->active = save && !cond && !cunk;
    trender(t, s, &stop);
    if (t->fail)
      return;
  }
  t->active = save;
  if (stop != STOP_ENDIF) {
    everr(t->err, t->errsz, "%{ if } without matching %{ endif }");
    t->fail = true;
  }
}

static void handle_for(struct trender *t, struct sbuf *s) {
  char *kvar = NULL, *vvar = dir_ident(t);
  if (vvar == NULL) {
    everr(t->err, t->errsz, "expected a variable in %{ for }");
    t->fail = true;
    return;
  }
  while (t->p < t->end && isspace((unsigned char)*t->p))
    t->p++;
  if (t->p < t->end && *t->p == ',') {
    t->p++;
    kvar = vvar;
    vvar = dir_ident(t);
    if (vvar == NULL) {
      free(kvar);
      everr(t->err, t->errsz, "expected a second variable in %{ for }");
      t->fail = true;
      return;
    }
  }
  char *inkw = dir_ident(t);
  bool bad_in = (inkw == NULL || strcmp(inkw, "in") != 0);
  free(inkw);
  if (bad_in) {
    free(kvar);
    free(vvar);
    everr(t->err, t->errsz, "expected 'in' in %{ for }");
    t->fail = true;
    return;
  }
  const char *close = brace_match(t->p, t->end);
  if (close == NULL) {
    free(kvar);
    free(vvar);
    everr(t->err, t->errsz, "unterminated %{ for } directive");
    t->fail = true;
    return;
  }
  const char *eend = close;
  bool rt = (eend > t->p && eend[-1] == '~'); /* `~}` strips following whitespace */
  if (rt)
    eend--;
  const char *exprS = t->p;
  size_t exprN = (size_t)(eend - t->p);
  t->p = close + 1;
  if (rt)
    t_rtrim(t);
  const char *bodyStart = t->p;
  enum dirstop stop = STOP_ENDFOR;

  if (!t->active) { /* traverse the body once, structurally */
    trender(t, s, &stop);
    free(kvar);
    free(vvar);
    if (!t->fail && stop != STOP_ENDFOR) {
      everr(t->err, t->errsz, "%{ for } without matching %{ endfor }");
      t->fail = true;
    }
    return;
  }

  hcl2_value *coll = hcl2_eval(exprS, exprN, t->ctx, t->err, t->errsz);
  if (coll == NULL) {
    free(kvar);
    free(vvar);
    t->fail = true;
    return;
  }
  if (is_unknown(coll)) { /* unknown collection: render nothing, result unknown */
    hcl2_value_free(coll);
    free(kvar);
    free(vvar);
    t->unknown = true;
    t->active = false;
    trender(t, s, &stop);
    t->active = true;
    if (!t->fail && stop != STOP_ENDFOR) {
      everr(t->err, t->errsz, "%{ for } without matching %{ endfor }");
      t->fail = true;
    }
    return;
  }
  if (coll->kind != HCL2_TUPLE && coll->kind != HCL2_OBJECT) {
    everr(t->err, t->errsz, "%{ for } requires a tuple or object");
    hcl2_value_free(coll);
    free(kvar);
    free(vvar);
    t->fail = true;
    return;
  }
  /* loop vars need a context to bind into; synthesize one if none was given */
  hcl2_ctx *prevctx = t->ctx;
  hcl2_ctx *tmp = NULL;
  if (t->ctx == NULL) {
    tmp = hcl2_ctx_new();
    if (tmp == NULL) {
      hcl2_value_free(coll);
      free(kvar);
      free(vvar);
      t->fail = true;
      return;
    }
    t->ctx = tmp;
  }
  hcl2_value *saved_v = ctx_take_var(t->ctx, vvar);
  hcl2_value *saved_k = kvar ? ctx_take_var(t->ctx, kvar) : NULL;
  size_t count = (coll->kind == HCL2_TUPLE) ? coll->n : coll->nf;

  if (count == 0) { /* still traverse the body once to consume %{ endfor } */
    t->active = false;
    trender(t, s, &stop);
    t->active = true;
  } else {
    for (size_t i = 0; i < count && !t->fail; i++) {
      hcl2_value *vval, *kval = NULL;
      if (coll->kind == HCL2_TUPLE) {
        vval = vclone(coll->items[i]);
        if (kvar)
          kval = hcl2_number((double)i);
      } else {
        vval = vclone(coll->fields[i].val);
        if (kvar)
          kval = hcl2_string(coll->fields[i].key);
      }
      if (vval == NULL || (kvar && kval == NULL) || !hcl2_ctx_set_var(t->ctx, vvar, vval)) {
        hcl2_value_free(vval);
        hcl2_value_free(kval);
        t->fail = true;
        break;
      }
      if (kvar && !hcl2_ctx_set_var(t->ctx, kvar, kval)) {
        hcl2_value_free(kval);
        t->fail = true;
        break;
      }
      t->p = bodyStart;
      trender(t, s, &stop);
    }
  }
  /* restore shadowed scope */
  hcl2_value_free(ctx_take_var(t->ctx, vvar));
  if (saved_v != NULL)
    hcl2_ctx_set_var(t->ctx, vvar, saved_v);
  if (kvar != NULL) {
    hcl2_value_free(ctx_take_var(t->ctx, kvar));
    if (saved_k != NULL)
      hcl2_ctx_set_var(t->ctx, kvar, saved_k);
  }
  hcl2_ctx_free(tmp);
  t->ctx = prevctx;
  hcl2_value_free(coll);
  free(kvar);
  free(vvar);
  if (!t->fail && stop != STOP_ENDFOR) {
    everr(t->err, t->errsz, "%{ for } without matching %{ endfor }");
    t->fail = true;
  }
}

static void trender(struct trender *t, struct sbuf *s, enum dirstop *stop) {
  *stop = STOP_END;
  while (t->p < t->end && !t->fail) {
    const char *p = t->p;
    if (p[0] == '$' && p + 2 < t->end && p[1] == '$' && p[2] == '{') {
      if (t->active)
        sb_puts(s, "${");
      t->p += 3;
      continue;
    }
    if (p[0] == '%' && p + 2 < t->end && p[1] == '%' && p[2] == '{') {
      if (t->active)
        sb_puts(s, "%{");
      t->p += 3;
      continue;
    }
    if (p[0] == '$' && p + 1 < t->end && p[1] == '{') {
      const char *close = brace_match(p + 2, t->end);
      if (close == NULL) {
        everr(t->err, t->errsz, "unterminated ${ ... } in template");
        t->fail = true;
        return;
      }
      const char *cs = p + 2, *ce = close;
      if (cs < ce && *cs == '~') { /* `${~` strips preceding whitespace */
        if (t->active)
          sb_rtrim(s);
        cs++;
      }
      bool rt = (ce > cs && ce[-1] == '~'); /* `~}` strips following whitespace */
      if (rt)
        ce--;
      if (t->active) {
        hcl2_value *iv = hcl2_eval(cs, (size_t)(ce - cs), t->ctx, t->err, t->errsz);
        if (iv == NULL) {
          t->fail = true;
          return;
        }
        if (iv->kind == HCL2_UNKNOWN) { /* unknown interpolation -> unknown result */
          t->unknown = true;
          hcl2_value_free(iv);
        } else {
          bool ok = val_to_text(iv, s, t->err, t->errsz);
          hcl2_value_free(iv);
          if (!ok) {
            t->fail = true;
            return;
          }
        }
      }
      t->p = close + 1;
      if (rt)
        t_rtrim(t);
      continue;
    }
    if (p[0] == '%' && p + 1 < t->end && p[1] == '{') {
      t->p += 2;
      if (t->p < t->end && *t->p == '~') { /* `%{~` strips preceding whitespace */
        if (t->active)
          sb_rtrim(s);
        t->p++;
      }
      while (t->p < t->end && isspace((unsigned char)*t->p))
        t->p++;
      const char *ks = t->p;
      while (t->p < t->end && isalpha((unsigned char)*t->p))
        t->p++;
      size_t kn = (size_t)(t->p - ks);
      if (kn == 2 && memcmp(ks, "if", 2) == 0) {
        handle_if(t, s);
      } else if (kn == 3 && memcmp(ks, "for", 3) == 0) {
        handle_for(t, s);
      } else if (kn == 4 && memcmp(ks, "else", 4) == 0) {
        skip_to_brace(t);
        *stop = STOP_ELSE;
        return;
      } else if (kn == 5 && memcmp(ks, "endif", 5) == 0) {
        skip_to_brace(t);
        *stop = STOP_ENDIF;
        return;
      } else if (kn == 6 && memcmp(ks, "endfor", 6) == 0) {
        skip_to_brace(t);
        *stop = STOP_ENDFOR;
        return;
      } else {
        everr(t->err, t->errsz, "unknown %{ } template directive");
        t->fail = true;
        return;
      }
      continue;
    }
    if (!t->heredoc && p[0] == '\\' && p + 1 < t->end) {
      char e = p[1];
      if (e == 'u' || e == 'U') { /* \uNNNN (BMP) and \UNNNNNNNN (full) escapes */
        int ndig = (e == 'u') ? 4 : 8;
        if (t->end - (p + 2) < ndig) {
          everr(t->err, t->errsz, "truncated unicode escape in string");
          t->fail = true;
          return;
        }
        uint32_t cp = 0;
        for (int i = 0; i < ndig; i++) {
          int h = hexval(p[2 + i]);
          if (h < 0) {
            everr(t->err, t->errsz, "invalid hex digit in unicode escape");
            t->fail = true;
            return;
          }
          cp = cp * 16 + (uint32_t)h;
        }
        if (t->active)
          sb_utf8(s, cp);
        t->p = p + 2 + ndig;
        continue;
      }
      char ch = e;
      if (e == 'n')
        ch = '\n';
      else if (e == 't')
        ch = '\t';
      else if (e == 'r')
        ch = '\r';
      if (t->active)
        sb_putc(s, ch);
      t->p += 2;
      continue;
    }
    if (t->active)
      sb_putc(s, *p);
    t->p++;
  }
}

/* Evaluate a string template (raw inner bytes). When `heredoc` is true the body
 * is a heredoc: backslash escapes are kept literal (only `${ }` / `$${` and the
 * `%{ }` directives are interpreted), matching HCL's template semantics. */
static hcl2_value *eval_template(const char *raw, bool heredoc, hcl2_ctx *ctx, char *err,
                                 size_t errsz) {
  struct sbuf s = {0};
  struct trender t = {.p = raw,
                      .end = raw + strlen(raw),
                      .heredoc = heredoc,
                      .active = true,
                      .unknown = false,
                      .ctx = ctx,
                      .err = err,
                      .errsz = errsz,
                      .fail = false};
  enum dirstop stop;
  trender(&t, &s, &stop);
  if (t.fail || s.oom) {
    free(s.p);
    return NULL;
  }
  if (stop != STOP_END) {
    everr(err, errsz, "stray %{ else/endif/endfor } in template");
    free(s.p);
    return NULL;
  }
  if (t.unknown) { /* an interpolated/conditional unknown makes the result unknown */
    free(s.p);
    return hcl2_unknown();
  }
  hcl2_value *v = hcl2_string(s.p ? s.p : "");
  free(s.p);
  return v;
}

static hcl2_value *eval_binary(enum tok op, hcl2_value *l, hcl2_value *r, char *err, size_t errsz,
                               int line, int col) {
  hcl2_value *res = NULL;
  if (is_unknown(l) || is_unknown(r)) { /* unknown propagates through operators */
    res = hcl2_unknown();
    goto done;
  }
  if (op == T_EQ || op == T_NE) {
    bool eq = vequal(l, r);
    res = hcl2_bool(op == T_EQ ? eq : !eq);
    goto done;
  }
  if (op == T_AND || op == T_OR) {
    if (l->kind != HCL2_BOOL || r->kind != HCL2_BOOL) {
      everr_at(err, errsz, "logical operators require booleans", line, col);
      goto done;
    }
    res = hcl2_bool(op == T_AND ? (l->b && r->b) : (l->b || r->b));
    goto done;
  }
  /* arithmetic + comparison: numbers */
  if (l->kind != HCL2_NUMBER || r->kind != HCL2_NUMBER) {
    everr_at(err, errsz, "arithmetic/comparison requires numbers", line, col);
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
      everr_at(err, errsz, "division by zero", line, col);
      break;
    }
    res = hcl2_number(a / b);
    break;
  case T_PCT:
    if (b == 0) {
      everr_at(err, errsz, "modulo by zero", line, col);
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
    everr_at(err, errsz, "unknown operator", line, col);
    break;
  }
done:
  hcl2_value_free(l);
  hcl2_value_free(r);
  return res;
}

/* Evaluate a for-expression (also the desugaring target of splat). Loop
 * variables are bound in the context for the duration of the loop and any
 * shadowed outer bindings are restored afterwards. */
static hcl2_value *eval_for(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz) {
  bool object = (x->kind == N_FOR_OBJECT);
  hcl2_value *coll = hcl2_eval_node(x->a, ctx, err, errsz);
  if (coll == NULL)
    return NULL;
  if (is_unknown(coll)) { /* can't iterate an unknown collection */
    hcl2_value_free(coll);
    return hcl2_unknown();
  }
  if (coll->kind != HCL2_TUPLE && coll->kind != HCL2_OBJECT) {
    everr(err, errsz, "for-expression requires a tuple or object");
    hcl2_value_free(coll);
    return NULL;
  }
  /* Loop vars must be bound somewhere; synthesize a context if none was given. */
  hcl2_ctx *tmp = NULL;
  hcl2_ctx *use = ctx;
  if (use == NULL) {
    tmp = hcl2_ctx_new();
    if (tmp == NULL) {
      hcl2_value_free(coll);
      return NULL;
    }
    use = tmp;
  }
  hcl2_value *saved_v = ctx_take_var(use, x->str);
  hcl2_value *saved_k = x->kvar ? ctx_take_var(use, x->kvar) : NULL;

  hcl2_value *result = object ? hcl2_object() : hcl2_tuple();
  bool ok = (result != NULL);
  size_t count = (coll->kind == HCL2_TUPLE) ? coll->n : coll->nf;
  for (size_t i = 0; ok && i < count; i++) {
    hcl2_value *vval, *kval = NULL;
    if (coll->kind == HCL2_TUPLE) {
      vval = vclone(coll->items[i]);
      if (x->kvar)
        kval = hcl2_number((double)i);
    } else {
      vval = vclone(coll->fields[i].val);
      if (x->kvar)
        kval = hcl2_string(coll->fields[i].key);
    }
    if (vval == NULL || (x->kvar && kval == NULL) || !hcl2_ctx_set_var(use, x->str, vval)) {
      hcl2_value_free(vval);
      hcl2_value_free(kval);
      ok = false;
      break;
    }
    if (x->kvar && !hcl2_ctx_set_var(use, x->kvar, kval)) {
      hcl2_value_free(kval);
      ok = false;
      break;
    }
    if (x->d != NULL) { /* optional 'if' filter */
      hcl2_value *c = hcl2_eval_node(x->d, use, err, errsz);
      if (c == NULL) {
        ok = false;
        break;
      }
      if (c->kind != HCL2_BOOL) {
        everr(err, errsz, "for-expression 'if' must be a boolean");
        hcl2_value_free(c);
        ok = false;
        break;
      }
      bool keep = c->b;
      hcl2_value_free(c);
      if (!keep)
        continue;
    }
    if (object) {
      hcl2_value *kk = hcl2_eval_node(x->b, use, err, errsz);
      if (kk == NULL) {
        ok = false;
        break;
      }
      if (kk->kind != HCL2_STRING) {
        everr(err, errsz, "object for-expression key must be a string");
        hcl2_value_free(kk);
        ok = false;
        break;
      }
      hcl2_value *vv = hcl2_eval_node(x->c, use, err, errsz);
      if (vv == NULL) {
        hcl2_value_free(kk);
        ok = false;
        break;
      }
      if (x->op == T_ELLIPSIS) { /* grouping: accumulate same-key values in a tuple */
        hcl2_value *grp = (hcl2_value *)hcl2_value_get(result, kk->str);
        if (grp == NULL) {
          grp = hcl2_tuple();
          if (grp == NULL || !hcl2_object_set(result, kk->str, grp)) {
            hcl2_value_free(grp);
            hcl2_value_free(kk);
            hcl2_value_free(vv);
            ok = false;
            break;
          }
        }
        if (!hcl2_tuple_push(grp, vv)) {
          hcl2_value_free(kk);
          hcl2_value_free(vv);
          ok = false;
          break;
        }
      } else if (!hcl2_object_set(result, kk->str, vv)) {
        hcl2_value_free(kk);
        hcl2_value_free(vv);
        ok = false;
        break;
      }
      hcl2_value_free(kk);
    } else {
      hcl2_value *bv = hcl2_eval_node(x->b, use, err, errsz);
      if (bv == NULL || !hcl2_tuple_push(result, bv)) {
        hcl2_value_free(bv);
        ok = false;
        break;
      }
    }
  }
  /* restore shadowed scope */
  hcl2_value_free(ctx_take_var(use, x->str));
  if (saved_v != NULL)
    hcl2_ctx_set_var(use, x->str, saved_v);
  if (x->kvar != NULL) {
    hcl2_value_free(ctx_take_var(use, x->kvar));
    if (saved_k != NULL)
      hcl2_ctx_set_var(use, x->kvar, saved_k);
  }
  hcl2_value_free(coll);
  hcl2_ctx_free(tmp);
  if (!ok) {
    hcl2_value_free(result);
    return NULL;
  }
  return result;
}

/* try(expr, ...): the first argument that evaluates without error (an unknown
 * counts as success and is returned). can(expr): whether expr evaluates without
 * error -> bool (or unknown if expr is unknown). Both are "special forms": their
 * arguments are evaluated lazily with errors suppressed, which a regular builtin
 * (whose arguments are pre-evaluated) cannot do. */
static hcl2_value *eval_try(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz) {
  if (x->n == 0) {
    everr_node(err, errsz, "try() needs at least one argument", x);
    return NULL;
  }
  for (size_t i = 0; i < x->n; i++) {
    char scratch[64] = "";
    hcl2_value *v = hcl2_eval_node(x->items[i], ctx, scratch, sizeof(scratch));
    if (v != NULL)
      return v; /* success (an unknown is a successful, if unknown, result) */
  }
  everr_node(err, errsz, "try(): all arguments produced errors", x);
  return NULL;
}
static hcl2_value *eval_can(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz) {
  if (x->n != 1) {
    everr_node(err, errsz, "can() takes 1 argument", x);
    return NULL;
  }
  char scratch[64] = "";
  hcl2_value *v = hcl2_eval_node(x->items[0], ctx, scratch, sizeof(scratch));
  if (v == NULL)
    return hcl2_bool(false);
  bool unk = is_unknown(v);
  hcl2_value_free(v);
  return unk ? hcl2_unknown() : hcl2_bool(true);
}

hcl2_value *hcl2_eval_node(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz) {
  switch (x->kind) {
  case N_LIT:
    return vclone(x->lit);
  case N_FOR_TUPLE:
  case N_FOR_OBJECT:
    return eval_for(x, ctx, err, errsz);
  case N_TEMPLATE:
    return eval_template(x->str, x->op == T_HEREDOC, ctx, err, errsz);
  case N_VAR: {
    const hcl2_value *v = ctx_var(ctx, x->str);
    if (v == NULL) {
      char m[160];
      snprintf(m, sizeof(m), "undefined variable \"%s\"", x->str);
      everr_node(err, errsz, m, x);
      return NULL;
    }
    return vclone(v);
  }
  case N_ATTR: {
    hcl2_value *o = hcl2_eval_node(x->a, ctx, err, errsz);
    if (o == NULL)
      return NULL;
    if (is_unknown(o)) {
      hcl2_value_free(o);
      return hcl2_unknown();
    }
    const hcl2_value *f = hcl2_value_get(o, x->str);
    if (f == NULL) {
      char m[160];
      snprintf(m, sizeof(m), "no attribute \"%s\"", x->str);
      everr_node(err, errsz, m, x);
      hcl2_value_free(o);
      return NULL;
    }
    hcl2_value *res = vclone(f);
    hcl2_value_free(o);
    return res;
  }
  case N_INDEX: {
    hcl2_value *base = hcl2_eval_node(x->a, ctx, err, errsz);
    if (base == NULL)
      return NULL;
    hcl2_value *idx = hcl2_eval_node(x->b, ctx, err, errsz);
    if (idx == NULL) {
      hcl2_value_free(base);
      return NULL;
    }
    if (is_unknown(base) || is_unknown(idx)) {
      hcl2_value_free(base);
      hcl2_value_free(idx);
      return hcl2_unknown();
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
      everr_node(err, errsz, "index out of range or wrong key/type", x);
    hcl2_value *res = f ? vclone(f) : NULL;
    hcl2_value_free(base);
    hcl2_value_free(idx);
    return res;
  }
  case N_UNARY: {
    hcl2_value *e = hcl2_eval_node(x->a, ctx, err, errsz);
    if (e == NULL)
      return NULL;
    if (is_unknown(e)) {
      hcl2_value_free(e);
      return hcl2_unknown();
    }
    hcl2_value *res = NULL;
    if (x->op == T_MINUS && e->kind == HCL2_NUMBER)
      res = hcl2_number(-e->num);
    else if (x->op == T_NOT && e->kind == HCL2_BOOL)
      res = hcl2_bool(!e->b);
    else
      everr_node(err, errsz, "unary operator type mismatch", x);
    hcl2_value_free(e);
    return res;
  }
  case N_BINARY: {
    hcl2_value *l = hcl2_eval_node(x->a, ctx, err, errsz);
    if (l == NULL)
      return NULL;
    hcl2_value *r = hcl2_eval_node(x->b, ctx, err, errsz);
    if (r == NULL) {
      hcl2_value_free(l);
      return NULL;
    }
    return eval_binary(x->op, l, r, err, errsz, x->line, x->col);
  }
  case N_COND: {
    hcl2_value *c = hcl2_eval_node(x->a, ctx, err, errsz);
    if (c == NULL)
      return NULL;
    if (is_unknown(c)) {
      hcl2_value_free(c);
      return hcl2_unknown();
    }
    if (c->kind != HCL2_BOOL) {
      everr(err, errsz, "condition must be a boolean");
      hcl2_value_free(c);
      return NULL;
    }
    bool t = c->b;
    hcl2_value_free(c);
    return hcl2_eval_node(t ? x->b : x->c, ctx, err, errsz);
  }
  case N_TUPLE: {
    hcl2_value *t = hcl2_tuple();
    if (!t)
      return NULL;
    for (size_t i = 0; i < x->n; i++) {
      hcl2_value *e = hcl2_eval_node(x->items[i], ctx, err, errsz);
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
      hcl2_value *v = hcl2_eval_node(x->items[i], ctx, err, errsz);
      if (v == NULL || !hcl2_object_set(o, x->keys[i], v)) {
        hcl2_value_free(v);
        hcl2_value_free(o);
        return NULL;
      }
    }
    return o;
  }
  case N_CALL: {
    /* try()/can() are special forms (lazy args); a caller may still override
       them by registering a context function of the same name. */
    if (ctx_func(ctx, x->str) == NULL) {
      if (strcmp(x->str, "try") == 0)
        return eval_try(x, ctx, err, errsz);
      if (strcmp(x->str, "can") == 0)
        return eval_can(x, ctx, err, errsz);
    }
    hcl2_func fn = ctx_func(ctx, x->str);
    if (fn == NULL)
      fn = builtin_func(x->str);
    if (fn == NULL) {
      char m[160];
      snprintf(m, sizeof(m), "unknown function \"%s\"", x->str);
      everr_node(err, errsz, m, x);
      return NULL;
    }
    hcl2_value **args = x->n ? calloc(x->n, sizeof(*args)) : NULL;
    if (x->n && args == NULL)
      return NULL;
    bool ok = true;
    size_t i = 0;
    for (; i < x->n; i++) {
      args[i] = hcl2_eval_node(x->items[i], ctx, err, errsz);
      if (args[i] == NULL) {
        ok = false;
        break;
      }
    }
    bool argunk = false;
    if (ok)
      for (size_t j = 0; j < x->n; j++)
        if (is_unknown(args[j])) {
          argunk = true;
          break;
        }
    hcl2_value *res = NULL;
    if (ok && argunk) {
      res = hcl2_unknown(); /* an unknown argument makes the call result unknown */
    } else if (ok && x->op == T_ELLIPSIS) {
      /* spread: the final argument must be a tuple whose elements become the
         trailing arguments. f(a, xs...) => f(a, xs[0], xs[1], ...) */
      hcl2_value *last = args[x->n - 1];
      if (last->kind != HCL2_TUPLE) {
        everr(err, errsz, "the '...' spread argument must be a tuple");
      } else {
        size_t fc = (x->n - 1) + last->n;
        hcl2_value **fa = fc ? calloc(fc, sizeof(*fa)) : NULL;
        if (fc == 0 || fa != NULL) {
          bool cok = true;
          for (size_t j = 0; j + 1 < x->n; j++)
            fa[j] = args[j]; /* borrowed; freed via args below */
          size_t made = 0;
          for (; cok && made < last->n; made++) {
            fa[x->n - 1 + made] = vclone(last->items[made]);
            if (fa[x->n - 1 + made] == NULL)
              cok = false;
          }
          if (cok)
            res = fn((const hcl2_value *const *)fa, fc, err, errsz);
          for (size_t j = 0; j < made; j++) /* free only the clones we own */
            hcl2_value_free(fa[x->n - 1 + j]);
          free(fa);
        }
      }
    } else if (ok) {
      res = fn((const hcl2_value *const *)args, x->n, err, errsz);
    }
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
  p.lx.start = src;
  p.lx.tokpos = src;
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
  hcl2_value *v = hcl2_eval_node(root, ctx, err, errsz);
  node_free(root);
  free(p.lx.text);
  return v;
}

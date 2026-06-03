/* Unit tests for c-hcl2 (M1 expressions, M2 bodies, M3 collection/template
 * features) plus an allocation fault-injection scan. Run via `make test`
 * (defines HCL2_FAULT_INJECT) or `make test SANITIZE=address`. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hcl2.h"

static int failures = 0;
static void check(const char *name, bool ok) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", name);
    failures++;
  } else {
    fprintf(stderr, "ok:   %s\n", name);
  }
}

static hcl2_value *ev(const char *s, hcl2_ctx *ctx) {
  char err[256];
  return hcl2_eval(s, strlen(s), ctx, err, sizeof(err));
}

/* number-equals helper */
static bool isnum(hcl2_value *v, double want) {
  double d;
  bool ok = v && hcl2_value_as_number(v, &d) && fabs(d - want) < 1e-9;
  hcl2_value_free(v);
  return ok;
}
static bool isstr(hcl2_value *v, const char *want) {
  const char *s = hcl2_value_as_string(v);
  bool ok = s && strcmp(s, want) == 0;
  hcl2_value_free(v);
  return ok;
}
static bool isbool(hcl2_value *v, bool want) {
  bool b, ok = v && hcl2_value_as_bool(v, &b) && b == want;
  hcl2_value_free(v);
  return ok;
}
static bool fails(const char *s, hcl2_ctx *ctx) {
  char err[256] = "";
  hcl2_value *v = hcl2_eval(s, strlen(s), ctx, err, sizeof(err));
  if (v) {
    hcl2_value_free(v);
    return false;
  }
  return err[0] != '\0';
}

/* Evaluate an expression, convert the result to `t` (taking ownership of t,
 * which is a no-op to free for primitive singletons), free the source, and
 * return the converted value so the isnum/isstr/isbool helpers can consume it. */
static hcl2_value *hcl2_convert_helper(const char *expr, hcl2_type *t) {
  char err[256] = "";
  hcl2_value *v = hcl2_eval(expr, strlen(expr), NULL, err, sizeof(err));
  hcl2_value *out = hcl2_convert(v, t, err, sizeof(err));
  hcl2_value_free(v);
  hcl2_type_free(t);
  return out;
}

/* True if `expr` evaluates to an unknown value against ctx. */
static bool evunk(const char *expr, hcl2_ctx *ctx) {
  char err[256] = "";
  hcl2_value *v = hcl2_eval(expr, strlen(expr), ctx, err, sizeof(err));
  bool r = (v != NULL && hcl2_value_is_unknown(v));
  hcl2_value_free(v);
  return r;
}

/* A caller-registered function, for hcl2_ctx_set_func coverage. */
static hcl2_value *fn_inc(const hcl2_value *const *a, size_t n, char *err, size_t errsz) {
  (void)err;
  (void)errsz;
  double d;
  if (n != 1 || !hcl2_value_as_number(a[0], &d))
    return NULL;
  return hcl2_number(d + 1);
}

#ifdef HCL2_FAULT_INJECT
/* Drive out-of-memory branches: fail the 1st allocation, then the 2nd, ... until
 * the input succeeds with no injected failure. Every NULL return must be clean
 * (verified under ASan: no leaks/UAF on any OOM path). Inputs must be valid so
 * the scan terminates. */
extern int hcl2_alloc_budget;
static bool oom_scan_expr(const char *s) {
  for (int b = 0; b <= 5000; b++) {
    hcl2_alloc_budget = b;
    char err[256] = "";
    hcl2_value *v = hcl2_eval(s, strlen(s), NULL, err, sizeof(err));
    hcl2_alloc_budget = -1;
    if (v) {
      hcl2_value_free(v);
      return true;
    }
  }
  return false;
}
static bool oom_scan_doc(const char *s) {
  for (int b = 0; b <= 5000; b++) {
    hcl2_alloc_budget = b;
    char err[256] = "";
    hcl2_doc *d = hcl2_parse(s, strlen(s), err, sizeof(err));
    hcl2_alloc_budget = -1;
    if (d) {
      hcl2_doc_free(d);
      return true;
    }
  }
  return false;
}
static bool oom_scan_json(const char *s) {
  for (int b = 0; b <= 5000; b++) {
    hcl2_alloc_budget = b;
    char err[256] = "";
    hcl2_value *v = hcl2_parse_json(s, strlen(s), err, sizeof(err));
    hcl2_alloc_budget = -1;
    if (v) {
      hcl2_value_free(v);
      return true;
    }
  }
  return false;
}
#endif

int main(void) {
  /* arithmetic + precedence */
  check("1+2*3", isnum(ev("1 + 2 * 3", NULL), 7));
  check("(1+2)*3", isnum(ev("(1 + 2) * 3", NULL), 9));
  check("7%3", isnum(ev("7 % 3", NULL), 1));
  check("unary minus", isnum(ev("-5 + 2", NULL), -3));
  check("neg literal in expr", isnum(ev("10 / -2", NULL), -5));

  /* comparison + logical + conditional */
  check("gt", isbool(ev("3 > 2", NULL), true));
  check("eq", isbool(ev("2 == 2", NULL), true));
  check("ne", isbool(ev("1 != 2", NULL), true));
  check("and", isbool(ev("true && false", NULL), false));
  check("or", isbool(ev("false || true", NULL), true));
  check("not", isbool(ev("!false", NULL), true));
  check("ternary", isnum(ev("1 < 2 ? 10 : 20", NULL), 10));
  check("ternary else", isnum(ev("1 > 2 ? 10 : 20", NULL), 20));
  check("string eq", isbool(ev("\"a\" == \"a\"", NULL), true));

  /* strings + templates */
  check("plain string", isstr(ev("\"hello\"", NULL), "hello"));
  check("interp number", isstr(ev("\"a${1 + 1}b\"", NULL), "a2b"));
  check("escaped dollar", isstr(ev("\"$${x}\"", NULL), "${x}"));
  check("escape n", isstr(ev("\"a\\nb\"", NULL), "a\nb"));
  check("interp nested object", isstr(ev("\"v=${ {a = 1}.a }\"", NULL), "v=1"));

  /* tuples + objects + traversal on literals */
  {
    hcl2_value *t = ev("[1, 2, 3]", NULL);
    check("tuple len", hcl2_value_len(t) == 3);
    double d;
    check("tuple[1]", hcl2_value_as_number(hcl2_value_at(t, 1), &d) && d == 2);
    hcl2_value_free(t);
  }
  check("tuple index expr", isnum(ev("[10, 20, 30][1]", NULL), 20));
  check("object attr", isnum(ev("{a = 1, b = 2}.b", NULL), 2));
  check("object index", isnum(ev("{a = 1, b = 2}[\"a\"]", NULL), 1));
  check("object colon syntax", isnum(ev("{a: 5}.a", NULL), 5));

  /* functions */
  check("length string", isnum(ev("length(\"abcd\")", NULL), 4));
  check("length tuple", isnum(ev("length([1, 2, 3])", NULL), 3));
  check("upper", isstr(ev("upper(\"hi\")", NULL), "HI"));
  check("lower", isstr(ev("lower(\"AB\")", NULL), "ab"));
  check("max", isnum(ev("max(1, 5, 3)", NULL), 5));
  check("min", isnum(ev("min(4, 2, 8)", NULL), 2));
  check("nested call", isnum(ev("length(upper(\"abc\"))", NULL), 3));

  /* context: variables + traversal */
  {
    hcl2_ctx *ctx = hcl2_ctx_new();
    hcl2_ctx_set_var(ctx, "x", hcl2_number(5));
    hcl2_ctx_set_var(ctx, "name", hcl2_string("world"));
    hcl2_value *obj = hcl2_object();
    hcl2_object_set(obj, "port", hcl2_number(8080));
    hcl2_ctx_set_var(ctx, "cfg", obj);
    hcl2_value *lst = hcl2_tuple();
    hcl2_tuple_push(lst, hcl2_string("a"));
    hcl2_tuple_push(lst, hcl2_string("b"));
    hcl2_ctx_set_var(ctx, "items", lst);

    check("var arith", isnum(ev("x * 2 + 1", ctx), 11));
    check("var template", isstr(ev("\"hi ${name}!\"", ctx), "hi world!"));
    check("var object attr", isnum(ev("cfg.port", ctx), 8080));
    check("var tuple index", isstr(ev("items[1]", ctx), "b"));
    check("var in condition", isbool(ev("x > 3 && length(items) == 2", ctx), true));

    /* custom function registration */
    hcl2_ctx_free(ctx);
  }

  /* errors */
  check("err undefined var", fails("nope", NULL));
  check("err type mismatch", fails("true + 1", NULL));
  check("err div zero", fails("1 / 0", NULL));
  check("err unknown fn", fails("frobnicate(1)", NULL));
  check("err parse", fails("1 +", NULL));
  check("err trailing", fails("1 2", NULL));
  check("err bad index", fails("[1, 2][9]", NULL));
  check("err cond not bool", fails("1 ? 2 : 3", NULL));
  check("err interpolate object", fails("\"x${ {a=1} }\"", NULL));
  check("err unterminated string", fails("\"abc", NULL));
  check("err unterminated interp", fails("\"a${1+1\"", NULL));

  /* M4 (partial): line/column diagnostics on syntax errors */
  {
    char err[256] = "";
    hcl2_value *v = hcl2_eval("1 + )", 5, NULL, err, sizeof(err));
    check("diag expr null", v == NULL);
    check("diag expr line/col", strstr(err, "line 1, column 5") != NULL);
  }
  {
    char err[256] = "";
    const char *src = "a = 1\nb = 2\nc = )\n";
    hcl2_doc *d = hcl2_parse(src, strlen(src), err, sizeof(err));
    check("diag doc null", d == NULL);
    check("diag doc line 3", strstr(err, "line 3, column 5") != NULL);
    hcl2_doc_free(d);
  }
  {
    char err[256] = "";
    hcl2_value *v = hcl2_eval("\"unterminated", 13, NULL, err, sizeof(err));
    check("diag string null", v == NULL);
    check("diag string col 1", strstr(err, "line 1, column 1") != NULL);
  }
  /* eval-error positions (carried on AST nodes, so they survive even deferred
     body decoding after the source buffer is gone) */
  {
    char err[256] = "";
    (void)hcl2_eval("1 +\n  nope", 10, NULL, err, sizeof(err));
    check("diag undef var pos", strstr(err, "line 2, column 3") != NULL);
    err[0] = '\0';
    (void)hcl2_eval("true + 1", 8, NULL, err, sizeof(err));
    check("diag type-mismatch pos", strstr(err, "line 1, column 6") != NULL);
    err[0] = '\0';
    (void)hcl2_eval("{a = 1}.b", 9, NULL, err, sizeof(err));
    check("diag no-attribute pos", strstr(err, "column") != NULL);
  }
  {
    /* deferred decode: parse, then evaluate an attribute whose expression
       references an undefined variable; the source string is a literal that is
       no longer the lexer's buffer at eval time. */
    char err[256] = "";
    const char *src = "x = 1\ny = 2\nz = undef_var + 1\n";
    hcl2_doc *d = hcl2_parse(src, strlen(src), err, sizeof(err));
    err[0] = '\0';
    hcl2_value *v = hcl2_body_attr_value(hcl2_doc_root(d), "z", NULL, err, sizeof(err));
    check("diag deferred null", v == NULL);
    check("diag deferred pos line 3", strstr(err, "line 3, column 5") != NULL);
    hcl2_doc_free(d);
  }

  /* M5 (partial): JSON value parsing */
  {
    char err[256] = "";
    const char *s = "{\"name\": \"demo\", \"port\": 8080, \"on\": true, "
                    "\"tags\": [\"a\", \"b\"], \"meta\": null}";
    hcl2_value *v = hcl2_parse_json(s, strlen(s), err, sizeof(err));
    check("json parse ok", v != NULL && hcl2_value_kind(v) == HCL2_OBJECT);
    check("json string", v && strcmp(hcl2_value_as_string(hcl2_value_get(v, "name")), "demo") == 0);
    double d;
    check("json number", v && hcl2_value_as_number(hcl2_value_get(v, "port"), &d) && d == 8080);
    bool b;
    check("json bool", v && hcl2_value_as_bool(hcl2_value_get(v, "on"), &b) && b);
    check("json null", v && hcl2_value_kind(hcl2_value_get(v, "meta")) == HCL2_NULL);
    const hcl2_value *tags = v ? hcl2_value_get(v, "tags") : NULL;
    check("json array", tags && hcl2_value_kind(tags) == HCL2_TUPLE && hcl2_value_len(tags) == 2);
    hcl2_value_free(v);
  }
  {
    char err[256] = "";
    const char *s = "[1, [2, 3], {\"k\": -4.5e1}]";
    hcl2_value *v = hcl2_parse_json(s, strlen(s), err, sizeof(err));
    double d;
    const hcl2_value *inner = v ? hcl2_value_at(v, 2) : NULL;
    check("json nested",
          inner && hcl2_value_as_number(hcl2_value_get(inner, "k"), &d) && d == -45.0);
    hcl2_value_free(v);
  }
  {
    const char *s = "\"a\\nb\\u0041\"";
    check("json escapes", isstr(hcl2_parse_json(s, strlen(s), NULL, 0), "a\nbA"));
    /* 3-byte UTF-8 from a \u escape: U+4E2D */
    const char *s3 = "\"\\u4e2d\"";
    check("json u 3-byte", isstr(hcl2_parse_json(s3, strlen(s3), NULL, 0), "\xe4\xb8\xad"));
  }
  {
    /* JSON parses into the value model, so the conversion layer applies */
    char err[256] = "";
    const char *s = "[\"1\", \"2\", \"2\"]";
    hcl2_value *src = hcl2_parse_json(s, strlen(s), err, sizeof(err));
    hcl2_type *t = hcl2_type_set(hcl2_type_number());
    hcl2_value *out = hcl2_convert(src, t, err, sizeof(err));
    check("json + convert", out && hcl2_value_len(out) == 2);
    hcl2_value_free(out);
    hcl2_value_free(src);
    hcl2_type_free(t);
  }
  {
    char err[256] = "";
    struct {
      const char *name, *src;
    } bad[] = {
        {"json err empty",            ""         },
        {"json err trailing",         "1 2"      },
        {"json err bad tok",          "}"        },
        {"json err unterminated str", "\"abc"    },
        {"json err no colon",         "{\"k\" 1}"},
        {"json err bad number",       "1.2.3"    },
        {"json err bad literal",      "tru"      },
        {"json err open array",       "[1,"      },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      err[0] = '\0';
      hcl2_value *v = hcl2_parse_json(bad[i].src, strlen(bad[i].src), err, sizeof(err));
      check(bad[i].name, v == NULL && err[0] != '\0');
      hcl2_value_free(v);
    }
  }

  /* M4 (partial): unknown values + propagation */
  {
    hcl2_ctx *ctx = hcl2_ctx_new();
    hcl2_ctx_set_var(ctx, "u", hcl2_unknown());
    hcl2_ctx_set_var(ctx, "n", hcl2_number(3));
    check("unknown is_unknown", evunk("u", ctx));
    check("unknown not for known", !evunk("n", ctx));
    check("unknown binary", evunk("u + 1", ctx));
    check("unknown compare", evunk("u > 1", ctx));
    check("unknown unary", evunk("-u", ctx));
    check("unknown cond", evunk("u ? 1 : 2", ctx));
    check("unknown attr", evunk("u.field", ctx));
    check("unknown index", evunk("u[0]", ctx));
    check("unknown call arg", evunk("length(u)", ctx));
    check("unknown call mixed", evunk("max(u, 1)", ctx));
    check("unknown for coll", evunk("[for x in u : x]", ctx));
    check("unknown ==", evunk("u == u", ctx));
    check("unknown template interp", evunk("\"v=${u}\"", ctx));
    check("unknown template if", evunk("\"%{ if u }a%{ endif }\"", ctx));
    check("unknown template for", evunk("\"%{ for x in u }a%{ endfor }\"", ctx));
    /* a known tuple may hold unknown elements (cty: the tuple stays known) */
    check("known tuple of unknowns", !evunk("[for x in [1, 2] : x + u]", ctx));
    /* convert(unknown) -> unknown of any target type */
    {
      char err[256] = "";
      hcl2_value *u = hcl2_unknown();
      hcl2_value *out = hcl2_convert(u, hcl2_type_number(), err, sizeof(err));
      check("unknown converts to unknown", out && hcl2_value_is_unknown(out));
      hcl2_value_free(u);
      hcl2_value_free(out);
    }
    /* known arithmetic with no unknown stays known and correct */
    check("known stays known", isnum(ev("n + 1", ctx), 4));
    hcl2_ctx_free(ctx);
  }

  /* M4 (partial): type constraints & conversion */
  {
    /* primitive coercions */
    check("conv num->str", isstr(hcl2_convert_helper("42", hcl2_type_string()), "42"));
    check("conv bool->str", isstr(hcl2_convert_helper("true", hcl2_type_string()), "true"));
    check("conv str->num", isnum(hcl2_convert_helper("\"3.5\"", hcl2_type_number()), 3.5));
    check("conv str->bool", isbool(hcl2_convert_helper("\"false\"", hcl2_type_bool()), false));
    check("conv num->num id", isnum(hcl2_convert_helper("7", hcl2_type_number()), 7));
    check("conv any id", isnum(hcl2_convert_helper("9", hcl2_type_any()), 9));
    check("conv str->num bad", hcl2_convert_helper("\"abc\"", hcl2_type_number()) == NULL);
    check("conv num->bool bad", hcl2_convert_helper("1", hcl2_type_bool()) == NULL);

    /* list(number): tuple of mixed-but-coercible elements -> tuple of numbers */
    {
      char err[256] = "";
      hcl2_value *src = ev("[1, \"2\", 3]", NULL);
      hcl2_type *t = hcl2_type_list(hcl2_type_number());
      hcl2_value *out = hcl2_convert(src, t, err, sizeof(err));
      check("conv list len", out && hcl2_value_len(out) == 3);
      double d;
      check("conv list elem coerced",
            out && hcl2_value_as_number(hcl2_value_at(out, 1), &d) && d == 2);
      hcl2_value_free(out);
      hcl2_value_free(src);
      hcl2_type_free(t);
    }
    /* set(string): de-duplicates */
    {
      char err[256] = "";
      hcl2_value *src = ev("[\"a\", \"b\", \"a\"]", NULL);
      hcl2_type *t = hcl2_type_set(hcl2_type_string());
      hcl2_value *out = hcl2_convert(src, t, err, sizeof(err));
      check("conv set dedup", out && hcl2_value_len(out) == 2);
      hcl2_value_free(out);
      hcl2_value_free(src);
      hcl2_type_free(t);
    }
    /* map(number): object values coerced */
    {
      char err[256] = "";
      hcl2_value *src = ev("{a = \"1\", b = 2}", NULL);
      hcl2_type *t = hcl2_type_map(hcl2_type_number());
      hcl2_value *out = hcl2_convert(src, t, err, sizeof(err));
      double d;
      check("conv map coerced",
            out && hcl2_value_as_number(hcl2_value_get(out, "a"), &d) && d == 1);
      hcl2_value_free(out);
      hcl2_value_free(src);
      hcl2_type_free(t);
    }
    /* nested list(list(number)) frees cleanly; shape error on non-tuple */
    {
      char err[256] = "";
      hcl2_type *t = hcl2_type_list(hcl2_type_list(hcl2_type_number()));
      hcl2_value *scalar = ev("5", NULL);
      check("conv list shape err", hcl2_convert(scalar, t, err, sizeof(err)) == NULL);
      hcl2_value_free(scalar);
      hcl2_type_free(t);
    }
    check("conv null args", hcl2_convert(NULL, hcl2_type_any(), NULL, 0) == NULL);
  }

  /* misc public API surface */
  {
    hcl2_value *n = ev("null", NULL);
    check("null kind", n != NULL && hcl2_value_kind(n) == HCL2_NULL);
    hcl2_value_free(n);
    hcl2_ctx *ctx = hcl2_ctx_new();
    check("set custom func", hcl2_ctx_set_func(ctx, "inc", fn_inc));
    check("custom func call", isnum(ev("inc(41)", ctx), 42));
    check("custom func over builtin", isnum(ev("inc(inc(0))", ctx), 2));
    hcl2_ctx_free(ctx);
  }

  /* ---- M3: for-expressions + splat ---- */
  check("for tuple map", isnum(ev("length([for x in [1,2,3] : x * 2])", NULL), 3));
  check("for tuple value", isnum(ev("[for x in [10,20,30] : x + 1][2]", NULL), 31));
  check("for tuple filter", isnum(ev("length([for x in [1,2,3,4] : x if x > 2])", NULL), 2));
  check("for tuple index var", isnum(ev("[for i, x in [5,6,7] : i + x][1]", NULL), 7));
  check("for over object values", isnum(ev("[for v in {a=1, b=2} : v][1]", NULL), 2));
  check("for object form", isnum(ev("{for k, v in {a=1, b=2} : k => v * 10}.b", NULL), 20));
  check("for object filter",
        isnum(ev("length({for k, v in {a=1,b=2,c=3} : k => v if v != 2})", NULL), 2));
  check("for nested", isnum(ev("[for xs in [[1,2],[3]] : length(xs)][0]", NULL), 2));
  {
    hcl2_ctx *ctx = hcl2_ctx_new();
    hcl2_value *people = hcl2_tuple();
    hcl2_value *p1 = hcl2_object();
    hcl2_object_set(p1, "name", hcl2_string("ada"));
    hcl2_value *p2 = hcl2_object();
    hcl2_object_set(p2, "name", hcl2_string("alan"));
    hcl2_tuple_push(people, p1);
    hcl2_tuple_push(people, p2);
    hcl2_ctx_set_var(ctx, "people", people);
    check("splat attr", isstr(ev("people[*].name[1]", ctx), "alan"));
    check("splat len", isnum(ev("length(people[*].name)", ctx), 2));
    /* loop var does not leak into the surrounding scope */
    check("for scope clean", fails("[for z in [1] : z][0] + z", ctx));
    hcl2_ctx_free(ctx);
  }
  check("for err not collection", fails("[for x in 5 : x]", NULL));
  check("for err if not bool", fails("[for x in [1] : x if 3]", NULL));
  check("for err obj key not string", fails("{for x in [1] : x => x}", NULL));
  check("for err missing in", fails("[for x [1] : x]", NULL));
  check("for err missing colon", fails("[for x in [1] x]", NULL));
  check("for err unterminated", fails("[for x in [1] : x", NULL));
  check("for err missing fatarrow", fails("{for k, v in {a=1} : k v}", NULL));

  /* ---- M3: variadic call spread (...) ---- */
  check("spread max", isnum(ev("max([1, 5, 3]...)", NULL), 5));
  check("spread min", isnum(ev("min([4, 2, 8]...)", NULL), 2));
  check("spread mixed args", isnum(ev("max(9, [1, 5]...)", NULL), 9));
  check("spread from for", isnum(ev("max([for x in [3, 7, 2] : x * 2]...)", NULL), 14));
  check("spread length one", isnum(ev("length([[1, 2, 3]][0])", NULL), 3));
  check("spread err not tuple", fails("max(5...)", NULL));
  check("spread err empty to max", fails("max([]...)", NULL));

  /* ---- M3: heredocs ---- */
  check("heredoc basic", isstr(ev("<<EOF\nhello\nworld\nEOF\n", NULL), "hello\nworld\n"));
  check("heredoc empty body", isstr(ev("<<EOF\nEOF\n", NULL), ""));
  check("heredoc interp", isstr(ev("<<EOF\nv=${1 + 2}\nEOF\n", NULL), "v=3\n"));
  check("heredoc keeps backslash", isstr(ev("<<EOF\na\\nb\nEOF\n", NULL), "a\\nb\n"));
  check("heredoc indented strip", isstr(ev("<<-EOF\n    a\n      b\n    EOF\n", NULL), "a\n  b\n"));
  check("heredoc err unterminated", fails("<<EOF\nhello\n", NULL));
  check("heredoc err no delim", fails("<<\nx\n", NULL));
  check("heredoc err no newline", fails("<<EOF x", NULL));
  {
    const char *src = "script = <<EOT\nline1\nline2\nEOT\n";
    char err[256] = "";
    hcl2_doc *doc = hcl2_parse(src, strlen(src), err, sizeof err);
    check("heredoc in body parses", doc != NULL);
    check("heredoc in body value",
          isstr(hcl2_body_attr_value(hcl2_doc_root(doc), "script", NULL, err, sizeof err),
                "line1\nline2\n"));
    hcl2_doc_free(doc);
  }

  /* ---- M3: template directives %{ if } / %{ for } ---- */
  check("dir if true", isstr(ev("\"%{ if true }yes%{ else }no%{ endif }\"", NULL), "yes"));
  check("dir if false", isstr(ev("\"%{ if false }yes%{ else }no%{ endif }\"", NULL), "no"));
  check("dir if no else", isstr(ev("\"a%{ if 1 > 2 }X%{ endif }b\"", NULL), "ab"));
  check("dir if cond expr", isstr(ev("\"%{ if 2 > 1 }big%{ endif }\"", NULL), "big"));
  check("dir for join", isstr(ev("\"%{ for n in [1,2,3] }${n},%{ endfor }\"", NULL), "1,2,3,"));
  check("dir for empty", isstr(ev("\"[%{ for n in [] }${n}%{ endfor }]\"", NULL), "[]"));
  check("dir for index var",
        isstr(ev("\"%{ for i, n in [9,8] }${i}:${n} %{ endfor }\"", NULL), "0:9 1:8 "));
  check("dir for object",
        isstr(ev("\"%{ for k, v in {a=1, b=2} }${k}=${v};%{ endfor }\"", NULL), "a=1;b=2;"));
  check(
      "dir nested for+if",
      isstr(ev("\"%{ for n in [1,2,3] }%{ if n == 2 }<${n}>%{ endif }%{ endfor }\"", NULL), "<2>"));
  check("dir escaped pct", isstr(ev("\"%%{x}\"", NULL), "%{x}"));
  check("dir for scope clean", fails("\"%{ for z in [1] }${z}%{ endfor }${z}\"", NULL));
  check("dir err missing endif", fails("\"%{ if true }x\"", NULL));
  check("dir err stray endif", fails("\"x%{ endif }\"", NULL));
  check("dir err unknown", fails("\"%{ while x }y%{ endwhile }\"", NULL));
  check("dir err if not bool", fails("\"%{ if 3 }x%{ endif }\"", NULL));
  check("dir err for not coll", fails("\"%{ for x in 5 }y%{ endfor }\"", NULL));
  check("dir err for missing in", fails("\"%{ for x [1] }y%{ endfor }\"", NULL));
  check("dir err unterminated for", fails("\"%{ for x in [1] }y\"", NULL));

  /* ---- M2: configuration bodies ---- */
  {
    const char *src = "# a comment\n"
                      "name     = \"demo\"   // trailing comment\n"
                      "replicas = 2 + 1\n"
                      "enabled  = true\n"
                      "/* block comment */\n"
                      "service \"api\" \"primary\" {\n"
                      "  port = base_port + 1\n"
                      "  upstream { host = \"10.0.0.1\" }\n"
                      "}\n"
                      "service \"web\" {\n"
                      "  port = 80\n"
                      "}\n";
    char err[256] = "";
    hcl2_doc *doc = hcl2_parse(src, strlen(src), err, sizeof(err));
    check("body parse ok", doc != NULL);
    const hcl2_body *root = hcl2_doc_root(doc);
    check("body root non-null", root != NULL);

    /* attributes (some need a context) */
    hcl2_ctx *ctx = hcl2_ctx_new();
    hcl2_ctx_set_var(ctx, "base_port", hcl2_number(8080));
    check("attr count", hcl2_body_attr_count(root) == 3);
    check("attr name 0", strcmp(hcl2_body_attr_name(root, 0), "name") == 0);
    check("attr name oob", hcl2_body_attr_name(root, 9) == NULL);
    check("has_attr yes", hcl2_body_has_attr(root, "replicas"));
    check("has_attr no", !hcl2_body_has_attr(root, "nope"));
    check("attr string", isstr(hcl2_body_attr_value(root, "name", ctx, err, sizeof err), "demo"));
    check("attr expr", isnum(hcl2_body_attr_value(root, "replicas", ctx, err, sizeof err), 3));
    check("attr bool", isbool(hcl2_body_attr_value(root, "enabled", ctx, err, sizeof err), true));
    check("attr missing", hcl2_body_attr_value(root, "ghost", ctx, err, sizeof err) == NULL);

    /* blocks */
    check("block count typed", hcl2_body_block_count(root, "service") == 2);
    check("block count all", hcl2_body_block_count(root, NULL) == 2);
    check("block count none", hcl2_body_block_count(root, "missing") == 0);
    const hcl2_block *svc = hcl2_body_block_at(root, "service", 0);
    check("block at", svc != NULL);
    check("block type", strcmp(hcl2_block_type(svc), "service") == 0);
    check("block labels", hcl2_block_label_count(svc) == 2);
    check("block label 0", strcmp(hcl2_block_label(svc, 0), "api") == 0);
    check("block label 1", strcmp(hcl2_block_label(svc, 1), "primary") == 0);
    check("block label oob", hcl2_block_label(svc, 5) == NULL);
    check("block at oob", hcl2_body_block_at(root, "service", 9) == NULL);

    /* nested body + attribute decoded against the same context */
    const hcl2_body *sb = hcl2_block_body(svc);
    check("nested body", sb != NULL);
    check("nested attr expr", isnum(hcl2_body_attr_value(sb, "port", ctx, err, sizeof err), 8081));
    const hcl2_block *up = hcl2_body_block_at(sb, "upstream", 0);
    check("nested block no labels", up != NULL && hcl2_block_label_count(up) == 0);
    check(
        "nested-nested attr",
        isstr(hcl2_body_attr_value(hcl2_block_body(up), "host", ctx, err, sizeof err), "10.0.0.1"));

    /* second service block, label-less form resolved by index */
    const hcl2_block *web = hcl2_body_block_at(root, "service", 1);
    check("second block label", strcmp(hcl2_block_label(web, 0), "web") == 0);

    hcl2_ctx_free(ctx);
    hcl2_doc_free(doc);
  }

  /* empty + accessor null-safety */
  {
    char err[256] = "";
    hcl2_doc *empty = hcl2_parse("", 0, err, sizeof err);
    check("empty body parse", empty != NULL);
    check("empty attr count", hcl2_body_attr_count(hcl2_doc_root(empty)) == 0);
    hcl2_doc_free(empty);
    check("null doc root", hcl2_doc_root(NULL) == NULL);
    check("null block accessors", hcl2_block_type(NULL) == NULL &&
                                      hcl2_block_label_count(NULL) == 0 &&
                                      hcl2_block_body(NULL) == NULL);
    hcl2_doc_free(NULL); /* no-op */
  }

  /* body parse errors */
  {
    char err[256];
    struct {
      const char *name, *src;
    } bad[] = {
        {"bad: not a name",         "= 1"               },
        {"bad: missing brace",      "svc \"a\" port = 1"},
        {"bad: stray rbrace",       "}"                 },
        {"bad: unterminated block", "svc {"             },
        {"bad: bad attr expr",      "x = 1 +"           },
        {"bad: bad nested name",    "svc { = 1 }"       },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      err[0] = '\0';
      hcl2_doc *d = hcl2_parse(bad[i].src, strlen(bad[i].src), err, sizeof err);
      check(bad[i].name, d == NULL && err[0] != '\0');
      hcl2_doc_free(d);
    }
  }

#ifdef HCL2_FAULT_INJECT
  /* ---- allocation fault injection: every OOM path returns cleanly ---- */
  {
    const char *exprs[] = {
        "1 + 2 * 3",
        "!true",
        "\"a${1 + 1}b\\n\"",
        "[1, 2, 3]",
        "{a = 1, b = 2}",
        "{a: 1}.a",
        "max(1, 2, 3)",
        "length(\"abc\")",
        "[1,2][0]",
        "true ? 1 : 2",
        "[for x in [1, 2, 3] : x * 2 if x > 1]",
        "{for k, v in {a = 1, b = 2} : k => v}",
        "[[1, 2], [3]][0]",
        "max([1, 2, 3]...)",
        "max(9, [1, 5]...)",
        "\"%{ if true }${1}%{ else }no%{ endif }\"",
        "\"%{ for n in [1, 2, 3] }${n},%{ endfor }\"",
        "<<EOF\nhi ${1 + 1}\nEOF\n",
        "<<-EOF\n  a\n    b\n  EOF\n",
    };
    bool all = true;
    for (size_t i = 0; i < sizeof(exprs) / sizeof(exprs[0]); i++)
      all = oom_scan_expr(exprs[i]) && all;
    check("oom scan: expressions", all);

    const char *docs[] = {
        "name = \"x\"\nport = 1 + 1\n",
        "svc \"a\" \"b\" { v = [1, 2] inner { z = 3 } }\n",
        "# c\nlist = [for x in [1, 2] : x]\ntext = <<EOT\nhi\nEOT\n",
    };
    bool alld = true;
    for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++)
      alld = oom_scan_doc(docs[i]) && alld;
    check("oom scan: documents", alld);

    const char *jsons[] = {
        "{\"a\": 1, \"b\": [true, null, \"x\\u00e9\"], \"c\": {\"d\": -2.5e3}}",
        "[1, 2, 3]",
    };
    bool allj = true;
    for (size_t i = 0; i < sizeof(jsons) / sizeof(jsons[0]); i++)
      allj = oom_scan_json(jsons[i]) && allj;
    check("oom scan: json", allj);

    /* convert() OOM paths: build inputs with the budget off, then fail each
       allocation inside the conversion until it succeeds. */
    hcl2_value *lsrc = ev("[1, \"2\", 3]", NULL);
    hcl2_value *msrc = ev("{a = \"1\", b = 2}", NULL);
    hcl2_type *lt = hcl2_type_set(hcl2_type_number());
    hcl2_type *mt = hcl2_type_map(hcl2_type_string());
    bool cok = false;
    for (int b = 0; b <= 5000; b++) {
      hcl2_alloc_budget = b;
      char e[64] = "";
      hcl2_value *o1 = hcl2_convert(lsrc, lt, e, sizeof(e));
      hcl2_value *o2 = hcl2_convert(msrc, mt, e, sizeof(e));
      hcl2_alloc_budget = -1;
      bool done = (o1 != NULL && o2 != NULL);
      hcl2_value_free(o1);
      hcl2_value_free(o2);
      if (done) {
        cok = true;
        break;
      }
    }
    check("oom scan: convert", cok);
    hcl2_value_free(lsrc);
    hcl2_value_free(msrc);
    hcl2_type_free(lt);
    hcl2_type_free(mt);
  }
#endif

  if (failures == 0) {
    fprintf(stderr, "\nAll c-hcl2 tests passed.\n");
    return 0;
  }
  fprintf(stderr, "\n%d c-hcl2 test(s) FAILED.\n", failures);
  return 1;
}

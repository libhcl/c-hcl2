/* Unit tests for c-hcl2 (M1 expressions, M2 bodies, M3 collection/template
 * features) plus an allocation fault-injection scan. Run via `make test`
 * (defines HCL2_FAULT_INJECT) or `make test SANITIZE=address`. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
/* Non-owning string compare for borrowed values (e.g. hcl2_value_get results). */
static bool isstr_v(const hcl2_value *v, const char *want) {
  const char *s = hcl2_value_as_string(v);
  return s && strcmp(s, want) == 0;
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

static bool hcl2_parse_fails(const char *s) {
  char err[256] = "";
  hcl2_doc *d = hcl2_parse(s, strlen(s), err, sizeof(err));
  if (d) {
    hcl2_doc_free(d);
    return false;
  }
  return err[0] != '\0';
}
static hcl2_value *hcl2_parse_json_helper(const char *s) {
  return hcl2_parse_json(s, strlen(s), NULL, 0);
}
static bool hcl2_parse_json_ok(const char *s) {
  hcl2_value *v = hcl2_parse_json(s, strlen(s), NULL, 0);
  bool ok = (v != NULL);
  hcl2_value_free(v);
  return ok;
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
  bool succeeded = false;
  for (int b = 0; b <= 5000; b++) {
    hcl2_alloc_budget = b;
    char err[256] = "";
    hcl2_doc *d = hcl2_parse(s, strlen(s), err, sizeof(err));
    hcl2_alloc_budget = -1;
    if (d) {
      hcl2_doc_free(d);
      succeeded = true;
    }
  }
  return succeeded;
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
/* OOM scan for the JSON-eval profile (template strings evaluated against a
 * ctx): drives the per-string template renderer's allocation arms too. */
static bool oom_scan_jsoneval(const char *s) {
  for (int b = 0; b <= 5000; b++) {
    hcl2_alloc_budget = b;
    hcl2_ctx *c = hcl2_ctx_new();
    char err[256] = "";
    hcl2_value *v = NULL;
    if (c != NULL) {
      /* binding may itself fail under OOM; that just means fewer vars resolve */
      hcl2_value *nv = hcl2_number(2);
      if (nv == NULL || !hcl2_ctx_set_var(c, "a", nv))
        hcl2_value_free(nv);
      v = hcl2_json_eval(s, strlen(s), c, err, sizeof(err));
    }
    hcl2_alloc_budget = -1;
    hcl2_ctx_free(c);
    if (v != NULL) {
      hcl2_value_free(v);
      return true;
    }
  }
  return false;
}
/* OOM scan for the schema-driven JSON body decoder: under each budget, build a
 * representative schema (labels + nested child + array blocks) and decode a
 * document, exercising every allocation-failure cleanup arm (schema build, node
 * synthesis, block label descent, make_block/free_block). */
static bool oom_scan_json_decode(const char *src) {
  for (int b = 0; b <= 5000; b++) {
    hcl2_alloc_budget = b;
    char err[256] = "";
    bool ok = false;
    hcl2_schema *child = hcl2_schema_new();
    hcl2_schema *top = hcl2_schema_new();
    bool sb = child != NULL && hcl2_schema_attr(child, "port", true) &&
              hcl2_schema_attr(child, "host", false);
    bool tattrs = top != NULL && hcl2_schema_attr(top, "name", true) &&
                  hcl2_schema_attr(top, "tags", false) && hcl2_schema_attr(top, "meta", false);
    bool tb = false;
    if (sb && tattrs)
      tb = hcl2_schema_block(top, "service", 1, child); /* consumes child */
    else
      hcl2_schema_free(child); /* never handed off to top */
    if (tb) {
      hcl2_doc *d = hcl2_json_decode(src, strlen(src), top, err, sizeof(err));
      if (d != NULL) {
        ok = true;
        hcl2_doc_free(d);
      }
    }
    hcl2_alloc_budget = -1;
    hcl2_schema_free(top);
    if (ok)
      return true;
  }
  return false;
}
/* Drive every allocation arm of the multi-error parser, including the
 * diagnostics-list growth and the recovery cleanup paths. parse_diags returns
 * a best-effort document on most budgets (NULL only when the body/doc calloc
 * itself fails), so we iterate the whole range rather than stopping early; the
 * point is that no budget leaks or crashes under ASan. */
static bool oom_scan_diags(const char *s) {
  for (int b = 0; b <= 5000; b++) {
    hcl2_alloc_budget = b;
    hcl2_diags *d = NULL;
    hcl2_doc *doc = hcl2_parse_diags(s, strlen(s), &d);
    hcl2_alloc_budget = -1;
    hcl2_diags_free(d);
    hcl2_doc_free(doc);
  }
  return true;
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
  check("escape quote+bs", isstr(ev("\"q\\\"\\\\\"", NULL), "q\"\\"));
  check("escape u BMP", isstr(ev("\"caf\\u00e9\"", NULL), "caf\xc3\xa9"));
  check("escape u 3-byte", isstr(ev("\"\\u4e2d\"", NULL), "\xe4\xb8\xad"));
  check("escape U 4-byte", isstr(ev("\"\\U0001F600\"", NULL), "\xf0\x9f\x98\x80"));
  check("escape u bad hex", fails("\"\\uZZZZ\"", NULL));
  check("escape u truncated", fails("\"\\u12\"", NULL));
  check("interp nested object", isstr(ev("\"v=${ {a = 1}.a }\"", NULL), "v=1"));
  check("interp nested string", isstr(ev("\"${ upper(\"hi\") }\"", NULL), "HI"));
  check("interp nested string args",
        isstr(ev("\"a${ join(\",\", [\"x\", \"y\"]) }b\"", NULL), "ax,yb"));
  check("interp ternary strings", isstr(ev("\"${ true ? \"yes\" : \"no\" }\"", NULL), "yes"));
  check("interp unterminated nested", fails("\"x${ \"y", NULL));

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

  /* full source spans via hcl2_expr_span (end is exclusive: past last token) */
  {
    int sl, sc, el, ec;
    char serr[128];
    check("span literal", hcl2_expr_span("42", 2, &sl, &sc, &el, &ec, serr, sizeof serr) &&
                              sl == 1 && sc == 1 && el == 1 && ec == 3);
    check("span call", hcl2_expr_span("foo(1, 2)", 9, &sl, &sc, &el, &ec, serr, sizeof serr) &&
                           sl == 1 && sc == 1 && el == 1 && ec == 10);
    check("span tuple", hcl2_expr_span("[1, 2, 3]", 9, &sl, &sc, &el, &ec, serr, sizeof serr) &&
                            sl == 1 && sc == 1 && el == 1 && ec == 10);
    const char *ml = "[\n  1,\n  2\n]";
    check("span multiline", hcl2_expr_span(ml, strlen(ml), &sl, &sc, &el, &ec, serr, sizeof serr) &&
                                sl == 1 && sc == 1 && el == 4 && ec == 2);
    check("span binary end",
          hcl2_expr_span("1 + 22", 6, &sl, &sc, &el, &ec, serr, sizeof serr) && el == 1 && ec == 7);
    check("span parse err", !hcl2_expr_span("1 +", 3, &sl, &sc, &el, &ec, serr, sizeof serr));
    check("span trailing err", !hcl2_expr_span("1 2", 3, &sl, &sc, &el, &ec, serr, sizeof serr));
    check("span null outptrs", hcl2_expr_span("7", 1, NULL, NULL, NULL, NULL, serr, sizeof serr));
  }

  /* eval-level unknown-type inference: operations on an unknown yield a typed
     unknown (singleton types compared by pointer identity) */
  {
    hcl2_ctx *c = hcl2_ctx_new();
    hcl2_ctx_set_var(c, "u", hcl2_unknown());
    char uerr[128];
    hcl2_value *v;
    v = hcl2_eval("u + 1", 5, c, uerr, sizeof uerr);
    check("unk arith -> number", v != NULL && hcl2_unknown_type(v) == hcl2_type_number());
    hcl2_value_free(v);
    v = hcl2_eval("u > 1", 5, c, uerr, sizeof uerr);
    check("unk compare -> bool", v != NULL && hcl2_unknown_type(v) == hcl2_type_bool());
    hcl2_value_free(v);
    v = hcl2_eval("u == 3", 6, c, uerr, sizeof uerr);
    check("unk eq -> bool", v != NULL && hcl2_unknown_type(v) == hcl2_type_bool());
    hcl2_value_free(v);
    v = hcl2_eval("!u", 2, c, uerr, sizeof uerr);
    check("unk not -> bool", v != NULL && hcl2_unknown_type(v) == hcl2_type_bool());
    hcl2_value_free(v);
    v = hcl2_eval("-u", 2, c, uerr, sizeof uerr);
    check("unk neg -> number", v != NULL && hcl2_unknown_type(v) == hcl2_type_number());
    hcl2_value_free(v);
    v = hcl2_eval("\"x${u}y\"", 8, c, uerr, sizeof uerr);
    check("unk template -> string", v != NULL && hcl2_unknown_type(v) == hcl2_type_string());
    hcl2_value_free(v);
    /* where the element type cannot be inferred, the unknown stays dynamic */
    v = hcl2_eval("u[0]", 4, c, uerr, sizeof uerr);
    check("unk index -> dynamic", v != NULL && hcl2_unknown_type(v) == hcl2_type_any());
    hcl2_value_free(v);
    hcl2_ctx_free(c);
  }

  /* try() / can() special forms */
  check("try first ok", isnum(ev("try(1 + 1)", NULL), 2));
  check("try fallback", isnum(ev("try(nope, 42)", NULL), 42));
  check("try chain", isstr(ev("try(nope, alsono, \"fb\")", NULL), "fb"));
  check("try optional attr", isnum(ev("try({a = 1}.b, 99)", NULL), 99));
  check("try all fail", fails("try(nope)", NULL));
  check("try zero args", fails("try()", NULL));
  check("can ok", isbool(ev("can(1 + 1)", NULL), true));
  check("can err", isbool(ev("can(nope)", NULL), false));
  check("can arity", fails("can(1, 2)", NULL));
  {
    hcl2_ctx *uc = hcl2_ctx_new();
    hcl2_ctx_set_var(uc, "u", hcl2_unknown());
    check("try unknown", evunk("try(u, 1)", uc));
    check("can unknown", evunk("can(u)", uc));
    hcl2_ctx_free(uc);
  }

  /* stdlib builtins */
  check("bi concat", isnum(ev("length(concat([1, 2], [3], [4, 5]))", NULL), 5));
  check("bi keys", isstr(ev("join(\",\", keys({b = 1, a = 2}))", NULL), "b,a"));
  check("bi values sum", isnum(ev("length(values({a = 1, b = 2}))", NULL), 2));
  check("bi contains yes", isbool(ev("contains([1, 2, 3], 2)", NULL), true));
  check("bi contains no", isbool(ev("contains([1, 2], 9)", NULL), false));
  check("bi lookup hit", isnum(ev("lookup({a = 1}, \"a\", 0)", NULL), 1));
  check("bi lookup miss", isnum(ev("lookup({a = 1}, \"z\", 7)", NULL), 7));
  check("bi coalesce", isnum(ev("coalesce(null, null, 3, 4)", NULL), 3));
  check("bi join", isstr(ev("join(\"-\", [\"a\", \"b\", \"c\"])", NULL), "a-b-c"));
  check("bi split", isnum(ev("length(split(\",\", \"a,b,c\"))", NULL), 3));
  check("bi split empty sep", isstr(ev("split(\"\", \"hi\")[0]", NULL), "hi"));
  check("bi abs", isnum(ev("abs(-5)", NULL), 5));
  check("bi floor", isnum(ev("floor(2.9)", NULL), 2));
  check("bi ceil", isnum(ev("ceil(2.1)", NULL), 3));
  /* numeric functions (Terraform / go-cty stdlib vectors) */
  check("bi signum neg", isnum(ev("signum(-13)", NULL), -1));
  check("bi signum zero", isnum(ev("signum(0)", NULL), 0));
  check("bi signum pos", isnum(ev("signum(13)", NULL), 1));
  check("bi log 2", isnum(ev("log(8, 2)", NULL), 3));
  check("bi log 10", isnum(ev("log(100, 10)", NULL), 2));
  check("bi pow", isnum(ev("pow(3, 2)", NULL), 9));
  check("bi pow zero", isnum(ev("pow(4, 0)", NULL), 1));
  check("bi parseint dec", isnum(ev("parseint(\"100\", 10)", NULL), 100));
  check("bi parseint hex", isnum(ev("parseint(\"FF\", 16)", NULL), 255));
  check("bi parseint neg", isnum(ev("parseint(\"-10\", 2)", NULL), -2));
  check("bi parseint b62", isnum(ev("parseint(\"Z\", 62)", NULL), 61));
  check("bi parseint bad", ev("parseint(\"12\", 2)", NULL) == NULL);
  /* string functions (Terraform vectors) */
  check("bi chomp", isstr(ev("chomp(\"hello\\n\")", NULL), "hello"));
  check("bi chomp crlf", isstr(ev("chomp(\"hi\\r\\n\")", NULL), "hi"));
  check("bi trimspace", isstr(ev("trimspace(\"  hi  \")", NULL), "hi"));
  check("bi trim", isstr(ev("trim(\"?!hello?!\", \"!?\")", NULL), "hello"));
  check("bi trimprefix", isstr(ev("trimprefix(\"helloworld\", \"hello\")", NULL), "world"));
  check("bi trimprefix miss", isstr(ev("trimprefix(\"helloworld\", \"x\")", NULL), "helloworld"));
  check("bi trimsuffix", isstr(ev("trimsuffix(\"helloworld\", \"world\")", NULL), "hello"));
  check("bi startswith yes", isbool(ev("startswith(\"hello\", \"he\")", NULL), true));
  check("bi startswith no", isbool(ev("startswith(\"hello\", \"lo\")", NULL), false));
  check("bi endswith yes", isbool(ev("endswith(\"hello\", \"lo\")", NULL), true));
  check("bi indent", isstr(ev("indent(2, \"a\\nb\")", NULL), "a\n  b"));
  /* UTF-8-aware string functions (Terraform vectors) */
  check("bi substr", isstr(ev("substr(\"hello world\", 1, 4)", NULL), "ello"));
  check("bi substr neg", isstr(ev("substr(\"hello world\", -5, -1)", NULL), "world"));
  check("bi substr toend", isstr(ev("substr(\"hello\", 2, -1)", NULL), "llo"));
  check("bi strrev", isstr(ev("strrev(\"hello\")", NULL), "olleh"));
  check("bi title", isstr(ev("title(\"hello world\")", NULL), "Hello World"));
  check("bi replace", isstr(ev("replace(\"1 + 2 + 3\", \"+\", \"-\")", NULL), "1 - 2 - 3"));
  check("bi replace none", isstr(ev("replace(\"abc\", \"x\", \"y\")", NULL), "abc"));
  check("bi length unicode", isnum(ev("length(\"héllo\")", NULL), 5));
  check("bi strrev unicode", isstr(ev("strrev(\"abé\")", NULL), "éba"));
  /* collection functions (Terraform vectors) */
  check("bi element", isnum(ev("element([\"a\", \"b\", \"c\"], 1) == \"b\" ? 1 : 0", NULL), 1));
  check("bi element wrap", isstr(ev("element([\"a\", \"b\"], 3)", NULL), "b"));
  check("bi slice",
        isstr(ev("join(\",\", slice([\"a\", \"b\", \"c\", \"d\"], 1, 3))", NULL), "b,c"));
  check("bi reverse", isstr(ev("join(\",\", reverse([\"a\", \"b\", \"c\"]))", NULL), "c,b,a"));
  check("bi sum", isnum(ev("sum([1, 2, 3, 4])", NULL), 10));
  check("bi range n", isstr(ev("join(\",\", [for x in range(3) : tostring(x)])", NULL), "0,1,2"));
  check("bi range start",
        isstr(ev("join(\",\", [for x in range(1, 4) : tostring(x)])", NULL), "1,2,3"));
  check("bi range step",
        isstr(ev("join(\",\", [for x in range(1, 8, 2) : tostring(x)])", NULL), "1,3,5,7"));
  check("bi range down",
        isstr(ev("join(\",\", [for x in range(3, 0, -1) : tostring(x)])", NULL), "3,2,1"));
  check("bi sort", isstr(ev("join(\",\", sort([\"c\", \"a\", \"b\"]))", NULL), "a,b,c"));
  check("bi distinct", isnum(ev("length(distinct([1, 2, 2, 3, 3, 3]))", NULL), 3));
  check("bi compact", isstr(ev("join(\",\", compact([\"a\", \"\", \"b\"]))", NULL), "a,b"));
  check("bi flatten", isnum(ev("length(flatten([[1, 2], [3], [[4, 5]]]))", NULL), 5));
  check("bi index", isnum(ev("index([\"a\", \"b\", \"c\"], \"c\")", NULL), 2));
  check("bi index miss", ev("index([\"a\"], \"z\")", NULL) == NULL);
  check("bi one", isnum(ev("one([42])", NULL), 42));
  check("bi one many", ev("one([1, 2])", NULL) == NULL);
  check("bi alltrue", isbool(ev("alltrue([true, true, true])", NULL), true));
  check("bi alltrue no", isbool(ev("alltrue([true, false])", NULL), false));
  check("bi anytrue", isbool(ev("anytrue([false, true])", NULL), true));
  check("bi coalescelist", isnum(ev("length(coalescelist([], [1, 2, 3]))", NULL), 3));
  /* map / object functions (Terraform vectors) */
  check("bi merge", isnum(ev("merge({a = 1, b = 2}, {b = 3, c = 4}).b", NULL), 3));
  check("bi merge len", isnum(ev("length(keys(merge({a = 1}, {b = 2}, {c = 3})))", NULL), 3));
  check("bi zipmap", isnum(ev("zipmap([\"a\", \"b\"], [1, 2]).b", NULL), 2));
  check("bi chunklist", isnum(ev("length(chunklist([1, 2, 3, 4, 5], 2))", NULL), 3));
  check("bi chunklist inner", isnum(ev("length(chunklist([1, 2, 3, 4, 5], 2)[0])", NULL), 2));
  check("bi matchkeys",
        isstr(ev("join(\",\", matchkeys([\"a\", \"b\", \"c\"], [1, 2, 3], [1, 3]))", NULL), "a,c"));
  /* conversion + set operations (Terraform vectors) */
  check("bi tolist", isnum(ev("length(tolist([1, 2, 3]))", NULL), 3));
  check("bi toset dedup", isnum(ev("length(toset([1, 1, 2, 2, 3]))", NULL), 3));
  check("bi tomap", isnum(ev("tomap({a = 1, b = 2}).b", NULL), 2));
  check("bi setunion", isnum(ev("length(setunion([1, 2], [2, 3], [3, 4]))", NULL), 4));
  check("bi setintersection", isnum(ev("length(setintersection([1, 2, 3], [2, 3, 4]))", NULL), 2));
  check("bi setsubtract", isnum(ev("length(setsubtract([1, 2, 3], [2]))", NULL), 2));
  check("bi setproduct", isnum(ev("length(setproduct([\"a\", \"b\"], [1, 2, 3]))", NULL), 6));
  check("bi setproduct inner",
        isstr(ev("setproduct([\"a\", \"b\"], [\"x\", \"y\"])[0][0]", NULL), "a"));
  check("bi transpose",
        isstr(ev("join(\",\", transpose({a = [\"a\", \"b\"], b = [\"b\", \"c\"]})[\"b\"])", NULL),
              "a,b"));
  /* format / formatlist / csvdecode (Terraform vectors) */
  check("bi format s", isstr(ev("format(\"Hello, %s!\", \"world\")", NULL), "Hello, world!"));
  check("bi format d", isstr(ev("format(\"%d apples\", 5)", NULL), "5 apples"));
  check("bi format f", isstr(ev("format(\"%.2f\", 3.14159)", NULL), "3.14"));
  check("bi format pad", isstr(ev("format(\"%05d\", 42)", NULL), "00042"));
  check("bi format v", isstr(ev("format(\"%v-%v-%v\", 1, true, \"x\")", NULL), "1-true-x"));
  check("bi format q", isstr(ev("format(\"%q\", \"hi\")", NULL), "\"hi\""));
  check("bi format idx", isstr(ev("format(\"%[2]s %[1]s\", \"a\", \"b\")", NULL), "b a"));
  check("bi format pct", isstr(ev("format(\"100%%\")", NULL), "100%"));
  check("bi format hex", isstr(ev("format(\"%x\", 255)", NULL), "ff"));
  check("bi formatlist",
        isstr(ev("join(\",\", formatlist(\"%s=%d\", [\"a\", \"b\"], [1, 2]))", NULL), "a=1,b=2"));
  check("bi formatlist scalar",
        isstr(ev("join(\",\", formatlist(\"%s-%s\", \"x\", [\"a\", \"b\"]))", NULL), "x-a,x-b"));
  check("bi csvdecode len", isnum(ev("length(csvdecode(\"a,b\\n1,2\\n3,4\"))", NULL), 2));
  check("bi csvdecode field", isstr(ev("csvdecode(\"a,b\\n1,2\")[0][\"b\"]", NULL), "2"));
  check("bi csvdecode quoted", isstr(ev("csvdecode(\"a\\n\\\"x,y\\\"\")[0][\"a\"]", NULL), "x,y"));
  /* regex / regexall / regex-replace (Terraform vectors) */
  check("bi regex whole", isstr(ev("regex(\"[a-z]+\", \"abc123\")", NULL), "abc"));
  check("bi regex group", isstr(ev("regex(\"([0-9]+)x([0-9]+)\", \"3x4\")[1]", NULL), "4"));
  check("bi regex named",
        isstr(ev("regex(\"(?P<y>[0-9]{4})-(?P<m>[0-9]{2})\", \"2026-06\")[\"m\"]", NULL), "06"));
  check("bi regex escd", isstr(ev("regex(\"\\\\d+\", \"x12y\")", NULL), "12"));
  check("bi regex anchor alt", isstr(ev("regex(\"^(?:cat|dog)$\", \"dog\")", NULL), "dog"));
  check("bi regex grp alt", isstr(ev("regex(\"^(cat|dog)$\", \"dog\")[0]", NULL), "dog"));
  check("bi regex repeat", isstr(ev("regex(\"a{2,3}\", \"aaaa\")", NULL), "aaa"));
  check("bi regex nomatch", ev("regex(\"z+\", \"abc\")", NULL) == NULL);
  check("bi regexall len", isnum(ev("length(regexall(\"[a-z]+\", \"a1b2c\"))", NULL), 3));
  check("bi regexall last", isstr(ev("regexall(\"[a-z]+\", \"a1b2c\")[2]", NULL), "c"));
  check("bi replace regex", isstr(ev("replace(\"foobar\", \"/o+/\", \"0\")", NULL), "f0bar"));
  check("bi replace regex grp",
        isstr(ev("replace(\"2026-06\", \"/([0-9]+)-([0-9]+)/\", \"$2/$1\")", NULL), "06/2026"));
  /* crypto / encoding (standard vectors) */
  check("bi base64encode", isstr(ev("base64encode(\"Hello World\")", NULL), "SGVsbG8gV29ybGQ="));
  check("bi base64decode", isstr(ev("base64decode(\"SGVsbG8gV29ybGQ=\")", NULL), "Hello World"));
  check("bi base64 round",
        isstr(ev("base64decode(base64encode(\"round-trip!\"))", NULL), "round-trip!"));
  check("bi md5", isstr(ev("md5(\"abc\")", NULL), "900150983cd24fb0d6963f7d28e17f72"));
  check("bi sha1", isstr(ev("sha1(\"abc\")", NULL), "a9993e364706816aba3e25717850c26c9cd0d89d"));
  check("bi sha256", isstr(ev("sha256(\"abc\")", NULL),
                           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  check("bi sha512", isstr(ev("sha512(\"abc\")", NULL),
                           "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                           "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
  check("bi base64sha256",
        isstr(ev("base64sha256(\"abc\")", NULL), "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0="));
  check("bi uuidv5 dns", isstr(ev("uuidv5(\"dns\", \"example.com\")", NULL),
                               "cfbff0d1-9375-5685-968c-48ce8b15ae17"));
  /* uuid() is random: validate its v4 format with the regex engine */
  check("bi uuid format", isbool(ev("can(regex(\"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-"
                                    "f]{3}-[0-9a-f]{12}$\", uuid())) "
                                    "&& uuid() != uuid()",
                                    NULL),
                                 true));
  /* cidr* — Terraform vectors (IPv4 + IPv6) */
  check("bi cidrhost v4", isstr(ev("cidrhost(\"10.12.127.0/20\", 16)", NULL), "10.12.112.16"));
  check("bi cidrhost v4 b", isstr(ev("cidrhost(\"10.12.127.0/20\", 268)", NULL), "10.12.113.12"));
  check("bi cidrhost v6", isstr(ev("cidrhost(\"2001:db8::/64\", 2)", NULL), "2001:db8::2"));
  check("bi cidrnetmask", isstr(ev("cidrnetmask(\"172.16.0.0/12\")", NULL), "255.240.0.0"));
  check("bi cidrsubnet v4",
        isstr(ev("cidrsubnet(\"172.16.0.0/12\", 4, 2)", NULL), "172.18.0.0/16"));
  check("bi cidrsubnet v4 b",
        isstr(ev("cidrsubnet(\"10.1.2.0/24\", 4, 15)", NULL), "10.1.2.240/28"));
  check("bi cidrsubnet v6", isstr(ev("cidrsubnet(\"fd00:fd12:3456:7890::/56\", 16, 162)", NULL),
                                  "fd00:fd12:3456:7800:a200::/72"));
  check("bi cidrsubnets len",
        isnum(ev("length(cidrsubnets(\"10.1.0.0/16\", 4, 4, 8, 4))", NULL), 4));
  check("bi cidrsubnets[0]",
        isstr(ev("cidrsubnets(\"10.1.0.0/16\", 4, 4, 8, 4)[0]", NULL), "10.1.0.0/20"));
  check("bi cidrsubnets[1]",
        isstr(ev("cidrsubnets(\"10.1.0.0/16\", 4, 4, 8, 4)[1]", NULL), "10.1.16.0/20"));
  check("bi cidrsubnets[2]",
        isstr(ev("cidrsubnets(\"10.1.0.0/16\", 4, 4, 8, 4)[2]", NULL), "10.1.32.0/24"));
  check("bi cidrsubnets[3]",
        isstr(ev("cidrsubnets(\"10.1.0.0/16\", 4, 4, 8, 4)[3]", NULL), "10.1.48.0/20"));
  check("bi cidrhost bad prefix", fails("cidrhost(\"10.0.0.0\", 1)", NULL));
  check("bi cidrnetmask v6 err", fails("cidrnetmask(\"2001:db8::/64\")", NULL));
  check("bi cidrsubnet overflow", fails("cidrsubnet(\"10.0.0.0/30\", 4, 1)", NULL));
  /* datetime — Terraform vectors */
  check("bi timeadd",
        isstr(ev("timeadd(\"2017-11-22T00:00:00Z\", \"10m\")", NULL), "2017-11-22T00:10:00Z"));
  check("bi timeadd hour neg",
        isstr(ev("timeadd(\"2017-11-22T00:00:00Z\", \"-1h30m\")", NULL), "2017-11-21T22:30:00Z"));
  check("bi timeadd day roll",
        isstr(ev("timeadd(\"2017-11-22T23:00:00Z\", \"2h\")", NULL), "2017-11-23T01:00:00Z"));
  check("bi timeadd keeps off", isstr(ev("timeadd(\"2017-11-22T00:00:00-02:00\", \"1h\")", NULL),
                                      "2017-11-22T01:00:00-02:00"));
  check("bi timecmp lt",
        isnum(ev("timecmp(\"2017-11-22T00:00:00Z\", \"2017-11-22T01:00:00Z\")", NULL), -1));
  check("bi timecmp eq",
        isnum(ev("timecmp(\"2017-11-22T00:00:00Z\", \"2017-11-22T00:00:00Z\")", NULL), 0));
  check("bi timecmp gt",
        isnum(ev("timecmp(\"2017-11-22T00:00:00Z\", \"2017-11-22T00:00:00-01:00\")", NULL), -1));
  check("bi formatdate",
        isstr(ev("formatdate(\"DD MMM YYYY hh:mm ZZZ\", \"2018-01-02T23:12:01Z\")", NULL),
              "02 Jan 2018 23:12 UTC"));
  check("bi formatdate 12h",
        isstr(ev("formatdate(\"HH:mmaa\", \"2018-01-02T23:12:01Z\")", NULL), "11:12pm"));
  check("bi formatdate weekday",
        isstr(ev("formatdate(\"EEEE, D MMMM YYYY\", \"2018-01-02T00:00:00Z\")", NULL),
              "Tuesday, 2 January 2018"));
  check("bi formatdate literal",
        isstr(ev("formatdate(\"YYYY 'on' MM\", \"2018-01-02T00:00:00Z\")", NULL), "2018 on 01"));
  check("bi timestamp format",
        isbool(ev("can(regex(\"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$\", "
                  "timestamp()))",
                  NULL),
               true));
  check("bi timeadd bad ts", fails("timeadd(\"nope\", \"1h\")", NULL));
  check("bi timeadd bad dur", fails("timeadd(\"2017-11-22T00:00:00Z\", \"1x\")", NULL));
  /* filesystem — lexical path functions (pure) */
  check("bi dirname", isstr(ev("dirname(\"foo/bar/baz.txt\")", NULL), "foo/bar"));
  check("bi dirname root", isstr(ev("dirname(\"/foo\")", NULL), "/"));
  check("bi dirname none", isstr(ev("dirname(\"file\")", NULL), "."));
  check("bi basename", isstr(ev("basename(\"foo/bar/baz.txt\")", NULL), "baz.txt"));
  check("bi basename root", isstr(ev("basename(\"/\")", NULL), "/"));
  check("bi basename trail", isstr(ev("basename(\"a/b/\")", NULL), "b"));
  check("bi abspath clean", isstr(ev("abspath(\"/a/b/../c/./d\")", NULL), "/a/c/d"));
  check("bi abspath rel", isbool(ev("startswith(abspath(\"x/y\"), \"/\")", NULL), true));
  setenv("HOME", "/home/tester", 1);
  check("bi pathexpand", isstr(ev("pathexpand(\"~/conf\")", NULL), "/home/tester/conf"));
  check("bi pathexpand tilde", isstr(ev("pathexpand(\"~\")", NULL), "/home/tester"));
  check("bi pathexpand none", isstr(ev("pathexpand(\"/no/tilde\")", NULL), "/no/tilde"));
  check("bi pathexpand user err", fails("pathexpand(\"~bob/x\")", NULL));
  /* file / fileexists / filebase64 against a real temp file */
  {
    const char *fp = "/tmp/c-hcl2-fstest.txt";
    FILE *tf = fopen(fp, "wb");
    check("fs tmpfile create", tf != NULL);
    if (tf != NULL) {
      fwrite("hello\n", 1, 6, tf);
      fclose(tf);
    }
    check("bi file", isstr(ev("file(\"/tmp/c-hcl2-fstest.txt\")", NULL), "hello\n"));
    check("bi fileexists true", isbool(ev("fileexists(\"/tmp/c-hcl2-fstest.txt\")", NULL), true));
    check("bi fileexists false",
          isbool(ev("fileexists(\"/tmp/c-hcl2-nope-xyz.txt\")", NULL), false));
    check("bi filebase64", isstr(ev("filebase64(\"/tmp/c-hcl2-fstest.txt\")", NULL), "aGVsbG8K"));
    check("bi file missing", fails("file(\"/tmp/c-hcl2-nope-xyz.txt\")", NULL));
    check("bi fileexists dir err", fails("fileexists(\"/tmp\")", NULL));
    remove(fp);
  }
  /* fileset — glob walk over a temp directory tree */
  {
    mkdir("/tmp/c-hcl2-fset", 0777);
    mkdir("/tmp/c-hcl2-fset/sub", 0777);
    const char *files[] = {"/tmp/c-hcl2-fset/a.txt", "/tmp/c-hcl2-fset/b.txt",
                           "/tmp/c-hcl2-fset/c.log", "/tmp/c-hcl2-fset/sub/d.txt",
                           "/tmp/c-hcl2-fset/sub/e.log"};
    for (size_t i = 0; i < 5; i++) {
      FILE *ff = fopen(files[i], "wb");
      if (ff != NULL) {
        fputc('x', ff);
        fclose(ff);
      }
    }
    check("bi fileset *.txt",
          isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"*.txt\"))", NULL), 2));
    check("bi fileset **", isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"**\"))", NULL), 5));
    check("bi fileset **/*.txt",
          isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"**/*.txt\"))", NULL), 3));
    check("bi fileset sub glob",
          isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"sub/*.log\"))", NULL), 1));
    check("bi fileset class",
          isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"[ab].txt\"))", NULL), 2));
    check("bi fileset brace ext",
          isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"*.{txt,log}\"))", NULL), 3));
    check("bi fileset brace names",
          isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"{a,b,c}.txt\"))", NULL), 2));
    check("bi fileset brace recursive",
          isnum(ev("length(fileset(\"/tmp/c-hcl2-fset\", \"**/{d,e}.{txt,log}\"))", NULL), 2));
    check("bi fileset contains",
          isbool(ev("contains(fileset(\"/tmp/c-hcl2-fset\", \"**/*.txt\"), \"sub/d.txt\")", NULL),
                 true));
    check("bi fileset bad dir", fails("fileset(\"/tmp/c-hcl2-nope-dir-xyz\", \"*\")", NULL));
    for (size_t i = 0; i < 5; i++)
      remove(files[i]);
    remove("/tmp/c-hcl2-fset/sub");
    remove("/tmp/c-hcl2-fset");
  }
  /* templatefile — evaluate a file as an HCL template with bound vars */
  {
    const char *tp = "/tmp/c-hcl2-tmpl.txt";
    FILE *tf = fopen(tp, "wb");
    if (tf != NULL) {
      const char *body = "Hi ${upper(name)}, %{ for n in nums ~}${n}.%{ endfor ~}";
      fwrite(body, 1, strlen(body), tf);
      fclose(tf);
    }
    check("bi templatefile",
          isstr(ev("templatefile(\"/tmp/c-hcl2-tmpl.txt\", {name = \"bob\", nums = [1, 2, 3]})",
                   NULL),
                "Hi BOB, 1.2.3."));
    remove(tp);
    /* a template that calls templatefile() recursively must error */
    const char *rp = "/tmp/c-hcl2-tmpl-rec.txt";
    FILE *rf = fopen(rp, "wb");
    if (rf != NULL) {
      const char *body = "${templatefile(\"x\", {})}";
      fwrite(body, 1, strlen(body), rf);
      fclose(rf);
    }
    check("bi templatefile no recursion",
          fails("templatefile(\"/tmp/c-hcl2-tmpl-rec.txt\", {})", NULL));
    remove(rp);
    check("bi templatefile bad vars", fails("templatefile(\"/tmp/c-hcl2-nope.txt\", [1])", NULL));
  }
  /* yamldecode — block, flow, scalars */
  check("bi yamldecode map", isstr(ev("yamldecode(\"foo: bar\").foo", NULL), "bar"));
  check("bi yamldecode num", isnum(ev("yamldecode(\"a: 1\\nb: 2\").b", NULL), 2));
  check("bi yamldecode seq", isnum(ev("yamldecode(\"list:\\n- 1\\n- 2\\n- 3\").list[1]", NULL), 2));
  check("bi yamldecode seq len",
        isnum(ev("length(yamldecode(\"list:\\n- 10\\n- 20\\n- 30\").list)", NULL), 3));
  check("bi yamldecode nested",
        isbool(ev("yamldecode(\"nested:\\n  x: true\\n  y: hi\").nested.x", NULL), true));
  check("bi yamldecode flow seq", isnum(ev("yamldecode(\"[10, 20, 30]\")[2]", NULL), 30));
  check("bi yamldecode flow map", isnum(ev("yamldecode(\"{a: 1, b: [2, 3]}\").b[0]", NULL), 2));
  check("bi yamldecode scalar num", isnum(ev("yamldecode(\"42\")", NULL), 42));
  check("bi yamldecode scalar bool", isbool(ev("yamldecode(\"true\")", NULL), true));
  check("bi yamldecode scalar str", isstr(ev("yamldecode(\"\\\"hi\\\"\")", NULL), "hi"));
  check("bi yamldecode null", isbool(ev("yamldecode(\"null\") == null", NULL), true));
  check("bi yamldecode quoted colon", isstr(ev("yamldecode(\"k: \\\"a: b\\\"\").k", NULL), "a: b"));
  check("bi yamldecode comment", isnum(ev("yamldecode(\"x: 1 # note\").x", NULL), 1));
  check(
      "bi yamldecode seq of maps",
      isstr(ev("yamldecode(\"items:\\n- name: a\\n  age: 1\\n- name: b\\n  age: 2\").items[1].name",
               NULL),
            "b"));
  check("bi yamldecode bad arg", fails("yamldecode(42)", NULL));
  /* yamlencode — block style, sorted keys, quoted strings */
  check("bi yamlencode num", isstr(ev("yamlencode(1)", NULL), "1\n"));
  check("bi yamlencode str", isstr(ev("yamlencode(\"foo\")", NULL), "\"foo\"\n"));
  check("bi yamlencode bool", isstr(ev("yamlencode(true)", NULL), "true\n"));
  check("bi yamlencode seq", isstr(ev("yamlencode([\"a\", \"b\"])", NULL), "- \"a\"\n- \"b\"\n"));
  check("bi yamlencode map sorted",
        isstr(ev("yamlencode({b = 2, a = 1})", NULL), "\"a\": 1\n\"b\": 2\n"));
  check("bi yamlencode seq under key",
        isstr(ev("yamlencode({foo = \"bar\", baz = [\"qux\"]})", NULL),
              "\"baz\":\n- \"qux\"\n\"foo\": \"bar\"\n"));
  check("bi yamlencode nested map",
        isstr(ev("yamlencode({a = {b = \"c\"}})", NULL), "\"a\":\n  \"b\": \"c\"\n"));
  check("bi yamlencode empty",
        isstr(ev("yamlencode({a = [], b = {}})", NULL), "\"a\": []\n\"b\": {}\n"));
  /* round-trip */
  check("bi yaml roundtrip", isnum(ev("yamldecode(yamlencode({n = 7})).n", NULL), 7));
  check("bi yaml roundtrip list",
        isstr(ev("yamldecode(yamlencode({xs = [\"p\", \"q\"]})).xs[1]", NULL), "q"));
  /* block scalars: literal | (clip / strip) and folded > */
  check("bi yaml block literal", isstr(ev("yamldecode(\"k: |\\n  a\\n  b\").k", NULL), "a\nb\n"));
  check("bi yaml block strip", isstr(ev("yamldecode(\"k: |-\\n  a\\n  b\").k", NULL), "a\nb"));
  check("bi yaml block folded", isstr(ev("yamldecode(\"k: >\\n  a\\n  b\").k", NULL), "a b\n"));
  check("bi yaml block in doc",
        isstr(ev("yamldecode(\"name: x\\nbody: |\\n  l1\\n  l2\\nport: 9\").body", NULL),
              "l1\nl2\n"));
  check("bi yaml block sibling",
        isnum(ev("yamldecode(\"name: x\\nbody: |\\n  l1\\n  l2\\nport: 9\").port", NULL), 9));
  /* anchors & aliases (&a / *a) and explicit tags (!!str / !!int) */
  check("bi yaml alias scalar", isnum(ev("yamldecode(\"base: &b 5\\nref: *b\").ref", NULL), 5));
  check("bi yaml alias coll len",
        isnum(ev("length(yamldecode(\"defs: &d [1, 2]\\nuse: *d\").use)", NULL), 2));
  check("bi yaml alias coll val",
        isnum(ev("yamldecode(\"defs: &d [1, 2]\\nuse: *d\").use[1]", NULL), 2));
  check("bi yaml anchor block",
        isnum(ev("yamldecode(\"anchored: &a\\n  x: 1\\nother: *a\").other.x", NULL), 1));
  check("bi yaml alias in seq", isnum(ev("yamldecode(\"- &i 9\\n- *i\")[1]", NULL), 9));
  check("bi yaml flow alias", isnum(ev("yamldecode(\"[&x 1, *x]\")[1]", NULL), 1));
  check("bi yaml undefined alias", fails("yamldecode(\"k: *nope\")", NULL));
  check("bi yaml tag str", isstr(ev("yamldecode(\"n: !!str 5\").n", NULL), "5"));
  check("bi yaml tag int", isnum(ev("yamldecode(\"n: !!int \\\"7\\\"\").n", NULL), 7));
  check("bi yaml tag bool", isbool(ev("yamldecode(\"n: !!bool \\\"true\\\"\").n", NULL), true));
  /* merge keys (<<) */
  check("bi yaml merge from",
        isnum(ev("yamldecode(\"base: &b {x: 1, y: 2}\\nderived:\\n  <<: *b\\n  y: 9\").derived.x",
                 NULL),
              1));
  check("bi yaml merge override",
        isnum(ev("yamldecode(\"base: &b {x: 1, y: 2}\\nderived:\\n  <<: *b\\n  y: 9\").derived.y",
                 NULL),
              9));
  check("bi yaml merge explicit-before",
        isnum(ev("yamldecode(\"base: &b {x: 1}\\nd:\\n  x: 5\\n  <<: *b\").d.x", NULL), 5));
  check(
      "bi yaml merge seq precedence",
      isnum(ev("yamldecode(\"a: &a {k: 1}\\nb: &b {k: 2, m: 3}\\nc:\\n  <<: [*a, *b]\").c.k", NULL),
            1));
  check(
      "bi yaml merge seq union",
      isnum(ev("yamldecode(\"a: &a {k: 1}\\nb: &b {k: 2, m: 3}\\nc:\\n  <<: [*a, *b]\").c.m", NULL),
            3));
  check("bi yaml merge bad value", fails("yamldecode(\"k:\\n  <<: 5\")", NULL));
  /* multi-document streams (beyond Terraform): a tuple of documents */
  check("bi yaml multidoc len",
        isnum(ev("length(yamldecode(\"---\\na: 1\\n---\\nb: 2\"))", NULL), 2));
  check("bi yaml multidoc[0]", isnum(ev("yamldecode(\"---\\na: 1\\n---\\nb: 2\")[0].a", NULL), 1));
  check("bi yaml multidoc[1]", isnum(ev("yamldecode(\"---\\na: 1\\n---\\nb: 2\")[1].b", NULL), 2));
  check("bi yaml multidoc no-lead", isnum(ev("yamldecode(\"a: 1\\n---\\nb: 2\")[1].b", NULL), 2));
  check("bi yaml single still value", isnum(ev("yamldecode(\"a: 1\\n...\").a", NULL), 1));
  check("bi yaml anchors per-doc scope", fails("yamldecode(\"a: &x 1\\n---\\nb: *x\")", NULL));
  check("bi tostring", isstr(ev("tostring(42)", NULL), "42"));
  check("bi tonumber", isnum(ev("tonumber(\"3.5\")", NULL), 3.5));
  check("bi tobool", isbool(ev("tobool(\"true\")", NULL), true));
  check("bi jsondecode", isnum(ev("jsondecode(\"{\\\"x\\\": 7}\").x", NULL), 7));
  check("bi jsonencode", isstr(ev("jsonencode([1, true, \"a\"])", NULL), "[1,true,\"a\"]"));
  check("bi jsonencode obj", isstr(ev("jsonencode({k = \"v\"})", NULL), "{\"k\":\"v\"}"));
  check("bi jsonencode roundtrip", isnum(ev("jsondecode(jsonencode({n = 5})).n", NULL), 5));
  /* builtin error cases */
  check("bi concat bad", fails("concat([1], 2)", NULL));
  check("bi join bad elem", fails("join(\",\", [1])", NULL));
  check("bi keys bad", fails("keys([1, 2])", NULL));
  check("bi abs bad", fails("abs(\"x\")", NULL));
  check("bi tonumber bad", fails("tonumber(\"abc\")", NULL));
  /* unknown still propagates through builtins (handled at the call site) */
  {
    hcl2_ctx *uc = hcl2_ctx_new();
    hcl2_ctx_set_var(uc, "u", hcl2_unknown());
    check("bi unknown arg", evunk("jsonencode(u)", uc));
    check("bi concat unknown", evunk("concat(u, [1])", uc));
    hcl2_ctx_free(uc);
  }

  /* M4: multi-error parsing with recovery */
  {
    const char *src = "a = 1\nb = )\nc = 2\nd = *\ne = 3\nsvc x {\n  bad = +\n}\nf = 4\n";
    hcl2_diags *d = NULL;
    hcl2_doc *doc = hcl2_parse_diags(src, strlen(src), &d);
    check("diags doc non-null", doc != NULL);
    check("diags collected 3", hcl2_diags_count(d) == 3);
    check("diag 0 line 2", strstr(hcl2_diags_msg(d, 0), "line 2") != NULL);
    check("diag 1 line 4", strstr(hcl2_diags_msg(d, 1), "line 4") != NULL);
    check("diag 2 line 7 (in block)", strstr(hcl2_diags_msg(d, 2), "line 7") != NULL);
    /* recovery kept every valid attribute */
    const hcl2_body *root = hcl2_doc_root(doc);
    check("recovered a", hcl2_body_has_attr(root, "a"));
    check("recovered c", hcl2_body_has_attr(root, "c"));
    check("recovered e", hcl2_body_has_attr(root, "e"));
    check("recovered f", hcl2_body_has_attr(root, "f"));
    check("diag msg oob NULL", hcl2_diags_msg(d, 99) == NULL);
    hcl2_diags_free(d);
    hcl2_doc_free(doc);
  }
  {
    /* a clean document yields zero diagnostics */
    const char *src = "x = 1\ny = 2\n";
    hcl2_diags *d = NULL;
    hcl2_doc *doc = hcl2_parse_diags(src, strlen(src), &d);
    check("clean diags 0", hcl2_diags_count(d) == 0);
    check("diags count NULL", hcl2_diags_count(NULL) == 0);
    hcl2_diags_free(d);
    hcl2_doc_free(doc);
  }
  {
    /* recovery from a non-name token at the start of a body */
    const char *src = "* nonsense\na = 1\n";
    hcl2_diags *d = NULL;
    hcl2_doc *doc = hcl2_parse_diags(src, strlen(src), &d);
    check("diags non-name 1", hcl2_diags_count(d) == 1);
    check("diags non-name recovered a", hcl2_body_has_attr(hcl2_doc_root(doc), "a"));
    hcl2_diags_free(d);
    hcl2_doc_free(doc);
  }
  {
    /* recovery skips a stray '}' at the top level */
    const char *src = "}\na = 1\n";
    hcl2_diags *d = NULL;
    hcl2_doc *doc = hcl2_parse_diags(src, strlen(src), &d);
    check("diags stray-rbrace 1", hcl2_diags_count(d) == 1);
    check("diags stray-rbrace recovered a", hcl2_body_has_attr(hcl2_doc_root(doc), "a"));
    hcl2_diags_free(d);
    hcl2_doc_free(doc);
  }
  {
    /* an unterminated block at EOF is reported but can't be recovered past */
    const char *src = "a = 1\nsvc {\n  b = 2\n";
    hcl2_diags *d = NULL;
    hcl2_doc *doc = hcl2_parse_diags(src, strlen(src), &d);
    check("diags eof-in-block >=1", hcl2_diags_count(d) >= 1);
    check("diags eof-in-block kept a", hcl2_body_has_attr(hcl2_doc_root(doc), "a"));
    hcl2_diags_free(d);
    hcl2_doc_free(doc);
  }
  {
    /* defensive accessor guards */
    check("diags_free NULL ok", (hcl2_diags_free(NULL), true));
    check("block_at NULL body", hcl2_body_block_at(NULL, NULL, 0) == NULL);
  }
  {
    /* regression: a lone '|'/'&'/invalid byte used to produce a non-advancing
     * T_ERR, so the recovery resync loop (parse_diags) spun forever. The lexer
     * now always consumes the offending byte; recovery must terminate. */
    const char *bad[] = {
        "{for k, v in {a = 1, =bo = 2} : k =|> v}", /* original fuzz find */
        "a = 1 | 2\nb = 3\n",
        "a = 1 & 2\nb = 3\n",
        "a = `\nb = 3\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      hcl2_diags *d = NULL;
      hcl2_doc *doc = hcl2_parse_diags(bad[i], strlen(bad[i]), &d);
      check("diags lone-op terminates", doc != NULL); /* and it returned at all */
      check("diags lone-op >=1 diag", hcl2_diags_count(d) >= 1);
      hcl2_diags_free(d);
      hcl2_doc_free(doc);
    }
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
    /* 2-byte \u (U+00E9) and the \/ \b \f escapes */
    const char *s2 = "\"\\u00e9\\/\\b\\f\"";
    check("json u 2-byte + esc", isstr(hcl2_parse_json(s2, strlen(s2), NULL, 0), "\xc3\xa9/\b\f"));
    /* a bad escape and a bad \u are errors */
    const char *sbe = "\"\\x\"";
    check("json bad escape", hcl2_parse_json(sbe, strlen(sbe), NULL, 0) == NULL);
    const char *sbu = "\"\\u00zz\"";
    check("json bad \\u", hcl2_parse_json(sbu, strlen(sbu), NULL, 0) == NULL);
  }
  {
    /* jsonencode covers every string-escape branch; a round-trip through
       jsondecode verifies both serialiser and parser on the same bytes. */
    hcl2_ctx *c = hcl2_ctx_new();
    hcl2_ctx_set_var(c, "s", hcl2_string("q\"b\\c\nd\re\tf\bg\fh\x01i"));
    check("jsonencode all escapes",
          isstr(ev("jsonencode(s)", c), "\"q\\\"b\\\\c\\nd\\re\\tf\\bg\\fh\\u0001i\""));
    hcl2_value *rt = ev("jsondecode(jsonencode(s))", c);
    check("json escape round-trip",
          rt && hcl2_value_as_string(rt) &&
              strcmp(hcl2_value_as_string(rt), "q\"b\\c\nd\re\tf\bg\fh\x01i") == 0);
    hcl2_value_free(rt);
    hcl2_ctx_free(c);
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
    /* JSON-eval profile: strings are HCL templates evaluated against a ctx. */
    char err[256] = "";
    hcl2_ctx *c = hcl2_ctx_new();
    hcl2_ctx_set_var(c, "name", hcl2_string("ada"));
    hcl2_ctx_set_var(c, "a", hcl2_number(2));
    hcl2_ctx_set_var(c, "b", hcl2_number(3));
    hcl2_ctx_set_var(c, "on", hcl2_bool(true));
    const char *src = "{\"greet\": \"hi ${name}\", \"sum\": \"${a + b}\", "
                      "\"lit\": 42, \"flag\": true, \"none\": null, "
                      "\"arr\": [\"${name}\", 1], \"nest\": {\"k\": \"v${a}\"}, "
                      "\"cond\": \"%{ if on }yes%{ else }no%{ endif }\"}";
    hcl2_value *v = hcl2_json_eval(src, strlen(src), c, err, sizeof(err));
    check("json_eval object", v != NULL && hcl2_value_kind(v) == HCL2_OBJECT);
    check("json_eval interp", v && isstr_v(hcl2_value_get(v, "greet"), "hi ada"));
    check("json_eval expr", v && isstr_v(hcl2_value_get(v, "sum"), "5"));
    check("json_eval number passthrough",
          v && hcl2_value_kind(hcl2_value_get(v, "lit")) == HCL2_NUMBER);
    check("json_eval bool passthrough",
          v && hcl2_value_kind(hcl2_value_get(v, "flag")) == HCL2_BOOL);
    check("json_eval null passthrough",
          v && hcl2_value_kind(hcl2_value_get(v, "none")) == HCL2_NULL);
    const hcl2_value *arr = v ? hcl2_value_get(v, "arr") : NULL;
    check("json_eval array",
          arr && hcl2_value_kind(arr) == HCL2_TUPLE && isstr_v(hcl2_value_at(arr, 0), "ada"));
    const hcl2_value *nest = v ? hcl2_value_get(v, "nest") : NULL;
    check("json_eval nested", nest && isstr_v(hcl2_value_get(nest, "k"), "v2"));
    check("json_eval directive", v && isstr_v(hcl2_value_get(v, "cond"), "yes"));
    hcl2_value_free(v);

    /* backslashes stay literal (JSON already un-escaped); $${ escapes ${ */
    const char *sb = "\"a\\\\b\""; /* JSON "a\\b" -> a\b -> literal a\b */
    hcl2_value *vb = hcl2_json_eval(sb, strlen(sb), c, err, sizeof(err));
    check("json_eval backslash literal", isstr_v(vb, "a\\b"));
    hcl2_value_free(vb);
    const char *se = "\"$${x}\""; /* -> literal ${x}, not interpolated */
    hcl2_value *ve = hcl2_json_eval(se, strlen(se), c, err, sizeof(err));
    check("json_eval dollar escape", isstr_v(ve, "${x}"));
    hcl2_value_free(ve);

    /* a broken interpolation is an error */
    const char *bad = "\"${\"";
    check("json_eval bad interp", hcl2_json_eval(bad, strlen(bad), c, err, sizeof(err)) == NULL);

    /* unknown var -> unknown result string */
    hcl2_ctx_set_var(c, "u", hcl2_unknown());
    const char *su = "\"x=${u}\"";
    hcl2_value *vu = hcl2_json_eval(su, strlen(su), c, err, sizeof(err));
    check("json_eval unknown interp", vu && hcl2_value_is_unknown(vu));
    hcl2_value_free(vu);
    hcl2_ctx_free(c);
  }
  /* M5: JSON body profile -- schema-driven attribute/block decoding */
  {
    char err[256] = "";
    /* schema: attrs name(req)/replicas/enabled/tags/meta; block service[1 label]
       with child attrs port(req)/host; block feature[0 labels] child attr on */
    hcl2_schema *child = hcl2_schema_new();
    hcl2_schema_attr(child, "port", true);
    hcl2_schema_attr(child, "host", false);
    hcl2_schema *feat = hcl2_schema_new();
    hcl2_schema_attr(feat, "on", false);
    hcl2_schema *top = hcl2_schema_new();
    hcl2_schema_attr(top, "name", true);
    hcl2_schema_attr(top, "replicas", false);
    hcl2_schema_attr(top, "enabled", false);
    hcl2_schema_attr(top, "tags", false);
    hcl2_schema_attr(top, "meta", false);
    hcl2_schema_attr(top, "nil", false);
    hcl2_schema_block(top, "service", 1, child);
    hcl2_schema_block(top, "feature", 0, feat);

    const char *src = "{"
                      "\"name\": \"app-${env}\","
                      "\"replicas\": 3,"
                      "\"enabled\": true,"
                      "\"tags\": [\"a\", \"b-${env}\"],"
                      "\"meta\": {\"k\": \"v\", \"n\": 2},"
                      "\"nil\": null,"
                      "\"service\": {"
                      "  \"api\": {\"port\": 8080, \"host\": \"h-${env}\"},"
                      "  \"web\": [{\"port\": 80}, {\"port\": 443}]"
                      "},"
                      "\"feature\": {\"on\": false}"
                      "}";
    hcl2_doc *doc = hcl2_json_decode(src, strlen(src), top, err, sizeof(err));
    check("json_decode ok", doc != NULL);
    const hcl2_body *root = hcl2_doc_root(doc);

    /* attributes are lazily evaluated against a ctx (string -> template) */
    hcl2_ctx *ctx = hcl2_ctx_new();
    hcl2_ctx_set_var(ctx, "env", hcl2_string("prod"));
    check("json_decode attr template",
          isstr(hcl2_body_attr_value(root, "name", ctx, err, sizeof(err)), "app-prod"));
    check("json_decode attr number",
          isnum(hcl2_body_attr_value(root, "replicas", ctx, err, sizeof(err)), 3));
    check("json_decode attr bool",
          isbool(hcl2_body_attr_value(root, "enabled", ctx, err, sizeof(err)), true));
    hcl2_value *tags = hcl2_body_attr_value(root, "tags", ctx, err, sizeof(err));
    check("json_decode attr array",
          tags && hcl2_value_len(tags) == 2 && isstr_v(hcl2_value_at(tags, 1), "b-prod"));
    hcl2_value_free(tags);
    hcl2_value *meta = hcl2_body_attr_value(root, "meta", ctx, err, sizeof(err));
    check("json_decode attr object", meta && isstr_v(hcl2_value_get(meta, "k"), "v"));
    hcl2_value_free(meta);
    hcl2_value *nil = hcl2_body_attr_value(root, "nil", ctx, err, sizeof(err));
    check("json_decode attr null", nil && hcl2_value_kind(nil) == HCL2_NULL);
    hcl2_value_free(nil);

    /* blocks: service has 3 (api + web x2); feature has 1 */
    check("json_decode block count", hcl2_body_block_count(root, "service") == 3);
    check("json_decode feature count", hcl2_body_block_count(root, "feature") == 1);
    const hcl2_block *api = hcl2_body_block_at(root, "service", 0);
    check("json_decode block label", api && strcmp(hcl2_block_label(api, 0), "api") == 0);
    check("json_decode nested attr",
          isnum(hcl2_body_attr_value(hcl2_block_body(api), "port", ctx, err, sizeof(err)), 8080));
    /* the two web blocks share the label "web" */
    const hcl2_block *web0 = hcl2_body_block_at(root, "service", 1);
    const hcl2_block *web1 = hcl2_body_block_at(root, "service", 2);
    check("json_decode array blocks labels", web0 && web1 &&
                                                 strcmp(hcl2_block_label(web0, 0), "web") == 0 &&
                                                 strcmp(hcl2_block_label(web1, 0), "web") == 0);
    check("json_decode array block bodies",
          isnum(hcl2_body_attr_value(hcl2_block_body(web1), "port", ctx, err, sizeof(err)), 443));
    const hcl2_block *feature = hcl2_body_block_at(root, "feature", 0);
    check("json_decode zero-label block", feature && hcl2_block_label_count(feature) == 0);

    hcl2_ctx_free(ctx);
    hcl2_doc_free(doc);
    hcl2_schema_free(top); /* frees child + feat too */
  }
  {
    /* JSON body decode error cases */
    char err[256] = "";
    hcl2_schema *s = hcl2_schema_new();
    hcl2_schema_attr(s, "name", true);
    hcl2_schema_block(s, "blk", 1, NULL); /* NULL child == empty body */
    struct {
      const char *name, *src;
    } bad[] = {
        {"jd top not object",        "[1, 2]"                                  },
        {"jd missing required",      "{\"other\": 1}"                          },
        {"jd unknown property",      "{\"name\": \"x\", \"bogus\": 1}"         },
        {"jd labels not object",     "{\"name\": \"x\", \"blk\": 5}"           },
        {"jd body not object",       "{\"name\": \"x\", \"blk\": {\"L\": 5}}"  },
        {"jd array elem not object", "{\"name\": \"x\", \"blk\": {\"L\": [1]}}"},
        {"jd bad json",              "{not json"                               },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
      check(bad[i].name,
            hcl2_json_decode(bad[i].src, strlen(bad[i].src), s, err, sizeof(err)) == NULL);
    /* NULL child schema with an empty body is OK */
    const char *okempty = "{\"name\": \"x\", \"blk\": {\"L\": {}}}";
    hcl2_doc *d2 = hcl2_json_decode(okempty, strlen(okempty), s, err, sizeof(err));
    check("jd empty body ok", d2 != NULL && hcl2_body_block_count(hcl2_doc_root(d2), "blk") == 1);
    hcl2_doc_free(d2);
    /* optional attribute simply absent */
    hcl2_schema *s2 = hcl2_schema_new();
    hcl2_schema_attr(s2, "name", true);
    hcl2_schema_attr(s2, "opt", false);
    const char *noopt = "{\"name\": \"x\"}";
    hcl2_doc *d3 = hcl2_json_decode(noopt, strlen(noopt), s2, err, sizeof(err));
    check("jd optional absent ok", d3 != NULL && !hcl2_body_has_attr(hcl2_doc_root(d3), "opt"));
    hcl2_doc_free(d3);
    hcl2_schema_free(s2);
    hcl2_schema_free(s);
    /* NULL schema decodes only an empty object */
    const char *empty = "{}";
    hcl2_doc *d4 = hcl2_json_decode(empty, strlen(empty), NULL, err, sizeof(err));
    check("jd null schema empty ok", d4 != NULL);
    hcl2_doc_free(d4);
    const char *nonempty = "{\"x\": 1}";
    check("jd null schema nonempty err",
          hcl2_json_decode(nonempty, strlen(nonempty), NULL, err, sizeof(err)) == NULL);
    /* defensive NULL-argument guards on the schema API */
    {
      hcl2_schema *s3 = hcl2_schema_new();
      check("schema_attr NULL s", !hcl2_schema_attr(NULL, "x", false));
      check("schema_attr NULL name", !hcl2_schema_attr(s3, NULL, false));
      check("schema_block NULL s", !hcl2_schema_block(NULL, "t", 0, hcl2_schema_new()));
      check("schema_block NULL type", !hcl2_schema_block(s3, NULL, 0, hcl2_schema_new()));
      hcl2_schema_free(s3);
    }
    check("schema_free NULL ok", (hcl2_schema_free(NULL), true));
  }
  {
    char err[256] = "";
    struct {
      const char *name, *src;
    } bad[] = {
        {"json err empty",            ""                                        },
        {"json err trailing",         "1 2"                                     },
        {"json err bad tok",          "}"                                       },
        {"json err unterminated str", "\"abc"                                   },
        {"json err no colon",         "{\"k\" 1}"                               },
        {"json err bad number",       "1.2.3"                                   },
        {"json err bad literal",      "tru"                                     },
        {"json err open array",       "[1,"                                     },
        {"json err number too long",
         "100000000000000000000000000000000000000000000000000000000000000000000"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      err[0] = '\0';
      hcl2_value *v = hcl2_parse_json(bad[i].src, strlen(bad[i].src), err, sizeof(err));
      check(bad[i].name, v == NULL && err[0] != '\0');
      hcl2_value_free(v);
    }
  }

  /* M4: distinct cty collection kinds (list/set/map) */
  {
    char err[256] = "";
    hcl2_value *src = ev("[1, \"2\", 2]", NULL);
    hcl2_type *lt = hcl2_type_list(hcl2_type_number());
    hcl2_value *l = hcl2_convert(src, lt, err, sizeof(err));
    check("convert -> list kind", l && hcl2_value_kind(l) == HCL2_LIST);
    check("list len", l && hcl2_value_len(l) == 3);
    double d;
    check("list elem coerced", l && hcl2_value_as_number(hcl2_value_at(l, 1), &d) && d == 2);
    hcl2_type *st = hcl2_type_set(hcl2_type_number());
    hcl2_value *s = hcl2_convert(src, st, err, sizeof(err));
    check("convert -> set kind", s && hcl2_value_kind(s) == HCL2_SET);
    check("set dedups", s && hcl2_value_len(s) == 2);
    hcl2_value_free(l);
    hcl2_value_free(s);
    hcl2_value_free(src);
    hcl2_type_free(lt);
    hcl2_type_free(st);

    hcl2_value *osrc = ev("{a = \"1\", b = 2}", NULL);
    hcl2_type *mt = hcl2_type_map(hcl2_type_number());
    hcl2_value *m = hcl2_convert(osrc, mt, err, sizeof(err));
    check("convert -> map kind", m && hcl2_value_kind(m) == HCL2_MAP);
    check("map get coerced", m && hcl2_value_as_number(hcl2_value_get(m, "a"), &d) && d == 1);
    /* list/map flow through the rest: length, for-expression, jsonencode */
    hcl2_ctx *c = hcl2_ctx_new();
    hcl2_value *lst = ev("[10, 20, 30]", NULL);
    hcl2_type *lt2 = hcl2_type_list(hcl2_type_number());
    hcl2_value *llv = hcl2_convert(lst, lt2, err, sizeof(err));
    hcl2_ctx_set_var(c, "l", llv); /* ctx owns llv */
    hcl2_ctx_set_var(c, "m", m);   /* ctx owns m */
    check("length(list)", isnum(ev("length(l)", c), 3));
    check("index list", isnum(ev("l[1]", c), 20));
    check("for over list", isnum(ev("[for x in l : x + 1][0]", c), 11));
    check("length(map)", isnum(ev("length(m)", c), 2));
    check("index map", isnum(ev("m[\"a\"]", c), 1));
    check("for over map values", isnum(ev("[for k, v in m : v][0]", c), 1));
    check("jsonencode list", isstr(ev("jsonencode(l)", c), "[10,20,30]"));
    check("jsonencode map", isstr(ev("jsonencode(m)", c), "{\"a\":1,\"b\":2}"));
    hcl2_value_free(lst);
    hcl2_value_free(osrc);
    hcl2_type_free(mt);
    hcl2_type_free(lt2);
    hcl2_ctx_free(c);
  }
  /* a list has a distinct kind from a tuple with the same elements */
  {
    char err[256] = "";
    hcl2_value *tup = ev("[1, 2]", NULL);
    hcl2_type *lt = hcl2_type_list(hcl2_type_number());
    hcl2_value *lst = hcl2_convert(tup, lt, err, sizeof(err));
    check("list kind != tuple kind",
          lst && hcl2_value_kind(lst) == HCL2_LIST && hcl2_value_kind(tup) == HCL2_TUPLE);
    hcl2_value_free(tup);
    hcl2_value_free(lst);
    hcl2_type_free(lt);
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
    /* typed unknowns: the type travels with the unknown */
    {
      char err[256] = "";
      /* a plain unknown is dynamic ("any"); a non-unknown has no unknown-type */
      hcl2_value *dyn = hcl2_unknown();
      check("dyn unknown type is any", hcl2_unknown_type(dyn) == hcl2_type_any());
      check("number has no unknown type", hcl2_unknown_type(hcl2_number(1)) == NULL);
      check("unknown_type NULL safe", hcl2_unknown_type(NULL) == NULL);
      hcl2_value_free(dyn);

      /* hcl2_unknown_of carries its type (primitive singletons compare by ptr) */
      hcl2_value *un = hcl2_unknown_of(hcl2_type_number());
      check("unknown_of is unknown", hcl2_value_is_unknown(un));
      check("unknown_of keeps type", hcl2_unknown_type(un) == hcl2_type_number());

      /* convert refines a dynamic unknown to the target type */
      hcl2_value *u = hcl2_unknown();
      hcl2_value *cv = hcl2_convert(u, hcl2_type_string(), err, sizeof(err));
      check("convert refines unknown", cv && hcl2_value_is_unknown(cv));
      check("convert refines type", hcl2_unknown_type(cv) == hcl2_type_string());

      /* convert to `any` is the identity: it preserves the carried type */
      hcl2_value *idv = hcl2_convert(un, hcl2_type_any(), err, sizeof(err));
      check("convert any preserves type", idv && hcl2_unknown_type(idv) == hcl2_type_number());

      /* convert to a collection type yields a typed (non-primitive) unknown */
      hcl2_type *lt = hcl2_type_list(hcl2_type_number());
      hcl2_value *lu = hcl2_convert(u, lt, err, sizeof(err));
      check("convert unknown to list", lu && hcl2_value_is_unknown(lu));
      check("convert unknown list typed",
            hcl2_unknown_type(lu) != NULL && hcl2_unknown_type(lu) != hcl2_type_any());
      hcl2_type_free(lt);

      /* clone keeps the type: binding a typed unknown and evaluating it clones */
      hcl2_ctx *tc = hcl2_ctx_new();
      hcl2_ctx_set_var(tc, "tu", hcl2_unknown_of(hcl2_type_string()));
      hcl2_value *ev_tu = ev("tu", tc);
      check("clone keeps unknown type", ev_tu && hcl2_unknown_type(ev_tu) == hcl2_type_string());
      hcl2_value_free(ev_tu);
      hcl2_ctx_free(tc);

      hcl2_value_free(un);
      hcl2_value_free(u);
      hcl2_value_free(cv);
      hcl2_value_free(idv);
      hcl2_value_free(lu);
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
    check("conv bool->bool id", isbool(hcl2_convert_helper("true", hcl2_type_bool()), true));
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

  /* ---- coverage: vequal across kinds (via == and set de-dup) ---- */
  check("eq null", isbool(ev("null == null", NULL), true));
  check("eq bool", isbool(ev("true == true", NULL), true));
  check("eq tuple", isbool(ev("[1, 2] == [1, 2]", NULL), true));
  check("eq tuple len", isbool(ev("[1] == [1, 2]", NULL), false));
  check("eq tuple elem", isbool(ev("[1, 2] == [1, 9]", NULL), false));
  check("eq object", isbool(ev("{a = 1, b = 2} == {a = 1, b = 2}", NULL), true));
  check("eq object nf", isbool(ev("{a = 1} == {a = 1, b = 2}", NULL), false));
  check("eq object key", isbool(ev("{a = 1} == {b = 1}", NULL), false));
  check("eq kind mismatch", isbool(ev("1 == \"1\"", NULL), false));
  {
    /* set de-dup of two unknowns exercises vequal's UNKNOWN case */
    char err[256] = "";
    hcl2_value *src = hcl2_tuple();
    hcl2_tuple_push(src, hcl2_unknown());
    hcl2_tuple_push(src, hcl2_unknown());
    hcl2_type *t = hcl2_type_set(hcl2_type_any());
    hcl2_value *out = hcl2_convert(src, t, err, sizeof(err));
    check("set dedup unknowns", out && hcl2_value_len(out) == 1);
    hcl2_value_free(out);
    hcl2_value_free(src);
    hcl2_type_free(t);
  }
  check("object literal dup key", isnum(ev("{a = 1, a = 2}.a", NULL), 2));

  /* ---- coverage: value-model inspector guards (public API) ---- */
  {
    hcl2_value *num = hcl2_number(5);
    check("len of non-collection", hcl2_value_len(num) == 0);
    check("at of non-tuple", hcl2_value_at(num, 0) == NULL);
    check("get of non-object", hcl2_value_get(num, "x") == NULL);
    check("push to non-tuple", !hcl2_tuple_push(num, hcl2_number(1)));
    check("set on non-object", !hcl2_object_set(num, "k", hcl2_number(1)));
    check("len NULL", hcl2_value_len(NULL) == 0);
    bool b;
    double d;
    check("as_bool wrong kind", !hcl2_value_as_bool(num, &b));
    check("as_number wrong kind",
          !hcl2_value_as_number(hcl2_value_at(hcl2_value_at(num, 0), 0), &d));
    hcl2_value *str = hcl2_string("x");
    check("as_number on string", !hcl2_value_as_number(str, &d));
    hcl2_value_free(str);
    hcl2_value_free(num);
  }
  /* lexer / json reachable edges */
  check("heredoc term trailing ws", isstr(ev("<<EOF\nx\nEOF  \n", NULL), "x\n"));
  check("heredoc indent all-blank", isstr(ev("<<-EOF\n\nEOF\n", NULL), "\n"));
  check("json uppercase \\u", isstr(hcl2_parse_json_helper("\"\\u00C9\""), "\xc3\x89"));
  check("json bad exponent", !hcl2_parse_json_ok("12e"));

  /* ---- coverage: a wrong-arg error for every builtin ---- */
  check("err length arity", fails("length()", NULL));
  check("err length type", fails("length(1)", NULL));
  check("err upper type", fails("upper(1)", NULL));
  check("err split type", fails("split(1, 2)", NULL));
  check("err min type", fails("min(\"a\")", NULL));
  check("err floor type", fails("floor(\"x\")", NULL));
  check("err values type", fails("values([1])", NULL));
  check("err contains type", fails("contains(1, 2)", NULL));
  check("err lookup type", fails("lookup(1, \"a\", 0)", NULL));
  check("err coalesce all null", fails("coalesce(null, null)", NULL));
  check("err join type", fails("join(1, [])", NULL));
  check("err jsondecode type", fails("jsondecode(1)", NULL));
  check("err tostring arity", fails("tostring(1, 2)", NULL));

  /* ---- coverage: operators, escapes, big output ---- */
  check("op le", isbool(ev("1 <= 1", NULL), true));
  check("op ge", isbool(ev("2 >= 3", NULL), false));
  check("op binary minus", isnum(ev("5 - 2", NULL), 3));
  check("op mod zero", fails("5 % 0", NULL));
  check("op logical non-bool", fails("true && 1", NULL));
  check("op unary minus type", fails("-true", NULL));
  check("op unary not type", fails("!1", NULL));
  check("op amp error", fails("1 & 2", NULL));
  check("op pipe error", fails("1 | 2", NULL));
  check("lex invalid char", fails("@", NULL));
  check("tmpl bool interp", isstr(ev("\"v=${true}\"", NULL), "v=true"));
  check("tmpl escape t r", isstr(ev("\"a\\tb\\rc\"", NULL), "a\tb\rc"));
  /* long join output (builtins sbuf growth) and a long literal template
     (evaluator sbuf growth) -- both exceed the 64-byte initial buffer */
  check("join grows sbuf",
        isnum(ev("length(join(\"\", [for x in [1,2,3,4,5,6,7,8,9,10] : \"0123456789\"]))", NULL),
              100));
  {
    const char *lit = /* ~140 chars -> sb doubles twice */
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"";
    hcl2_value *v = ev(lit, NULL);
    check("long literal grows sbuf",
          v && hcl2_value_as_string(v) && strlen(hcl2_value_as_string(v)) == 138);
    hcl2_value_free(v);
  }
  {
    /* a single large put (one interpolation / one join element) forces the
       `cap *= 2` doubling loop in both sbuf implementations */
    char big[200];
    memset(big, 'z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    hcl2_ctx *c = hcl2_ctx_new();
    hcl2_ctx_set_var(c, "big", hcl2_string(big));
    check("template big put", isnum(ev("length(\"<${big}>\")", c), 201));
    check("join big put", isnum(ev("length(join(\",\", [big, big]))", c), 399));
    hcl2_ctx_free(c);
  }

  /* for-expression / for-directive shadowing a bound context variable */
  {
    hcl2_ctx *c = hcl2_ctx_new();
    hcl2_ctx_set_var(c, "x", hcl2_number(99));
    check("for-expr shadows var", isnum(ev("[for x in [1, 2] : x][1]", c), 2));
    check("var restored after for", isnum(ev("x", c), 99));
    hcl2_ctx_set_var(c, "k", hcl2_string("outer"));
    check("for-dir shadows var", isstr(ev("\"%{ for k in [1, 2] }${k}%{ endfor }\"", c), "12"));
    check("var restored after dir", isstr(ev("k", c), "outer"));
    hcl2_ctx_free(c);
  }

  /* ---- coverage: parser error branches ---- */
  check("perr call no rparen", fails("length(1 2)", NULL));
  check("perr paren empty", fails("()", NULL));
  check("perr paren no close", fails("(1 2", NULL));
  check("perr tuple no comma", fails("[1 2]", NULL));
  check("perr object no eq", fails("{a 1}", NULL));
  check("perr index no close", fails("[1][0", NULL));
  check("perr splat no close", fails("[1][*x]", NULL));
  check("perr splat idx no close", fails("[[1]][*][0", NULL));
  check("perr cond no colon", fails("true ? 1", NULL));
  check("perr cond no else", fails("true ? 1 :", NULL));
  check("perr block no body", hcl2_parse_fails("svc \"a\""));
  check("perr heredoc no close brace", fails("\"%{ if true }x%{ endif\"", NULL));
  check("perr if unterminated", fails("\"%{ if true\"", NULL));
  check("perr for unterminated", fails("\"%{ for x in [1]\"", NULL));
  check("perr for no endfor", fails("\"%{ for x in [1] }y\"", NULL));

  /* ---- coverage: JSON edge + error branches ---- */
  check("json false literal", isbool(hcl2_parse_json_helper("false"), false));
  check("json empty array", hcl2_parse_json_ok("[]"));
  check("json empty object", hcl2_parse_json_ok("{}"));
  check("json key not string", !hcl2_parse_json_ok("{1: 2}"));
  check("json trailing backslash", !hcl2_parse_json_ok("\"a\\"));
  check("json u truncated", !hcl2_parse_json_ok("\"\\u12\""));
  check("json lone minus", !hcl2_parse_json_ok("-"));
  check("json array no comma", !hcl2_parse_json_ok("[1 2]"));
  check("json object no comma", !hcl2_parse_json_ok("{\"a\":1 \"b\":2}"));

  /* ---- coverage: convert edges + errors ---- */
  {
    char err[256] = "";
    hcl2_value *n = hcl2_number(5);
    hcl2_value *o1 = hcl2_convert(n, hcl2_type_number(), err, sizeof(err)); /* number->number */
    check("conv num id", o1 && hcl2_value_kind(o1) == HCL2_NUMBER);
    hcl2_value_free(o1);
    hcl2_value_free(n);
    hcl2_value *tup = ev("[1, 2]", NULL);
    hcl2_type *st = hcl2_type_string();
    check("conv tuple to string err", hcl2_convert(tup, st, err, sizeof(err)) == NULL);
    hcl2_type *mt = hcl2_type_map(hcl2_type_number());
    check("conv tuple to map err", hcl2_convert(tup, mt, err, sizeof(err)) == NULL);
    hcl2_value_free(tup);
    hcl2_type_free(mt);
  }

  /* ---- coverage: misc builtin/value guards ---- */
  check("jsonencode arity 0", fails("jsonencode()", NULL));
  check("jsonencode arity 2", fails("jsonencode(1, 2)", NULL));
  {
    hcl2_value *t = hcl2_tuple();
    check("push NULL elem", !hcl2_tuple_push(t, NULL));
    hcl2_value_free(t);
    check("has_attr NULL body", !hcl2_body_has_attr(NULL, "x"));
    check("block_count NULL", hcl2_body_block_count(NULL, NULL) == 0);
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
  check("for object grouping",
        isstr(ev("jsonencode({for s in [\"a\", \"b\", \"a\"] : s => s...})", NULL),
              "{\"a\":[\"a\",\"a\"],\"b\":[\"b\"]}"));
  check("for object group len",
        isnum(ev("length({for s in [\"a\", \"b\", \"a\"] : s => s...}.a)", NULL), 2));
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
    /* splat captures the whole following traversal (HCL semantics): the
       result is a tuple, inspected here via jsonencode/length/join. */
    check("splat attr", isstr(ev("jsonencode(people[*].name)", ctx), "[\"ada\",\"alan\"]"));
    check("splat len", isnum(ev("length(people[*].name)", ctx), 2));
    check("legacy splat .*", isstr(ev("join(\",\", people.*.name)", ctx), "ada,alan"));
    /* index trailer inside the splat: [*].a[0] maps per element -> [10, 30] */
    check("splat index trailer",
          isstr(ev("jsonencode([{a = [10, 20]}, {a = [30, 40]}][*].a[0])", NULL), "[10,30]"));
    /* chained splats now parse + evaluate as a nested splat per element */
    check("splat chained list",
          isstr(ev("jsonencode([[1, 2], [3, 4]][*][*])", NULL), "[[1,2],[3,4]]"));
    check("splat chained len", isnum(ev("length([[1, 2], [3, 4]][*][*])", NULL), 2));
    check("splat chained attr", isstr(ev("jsonencode(people.*.*)", ctx), "[[\"ada\"],[\"alan\"]]"));
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
  /* whitespace strip markers ${~ ~} / %{~ ~} */
  check("ws strip left", isstr(ev("\"a  ${~ 1 }b\"", NULL), "a1b"));
  check("ws strip right", isstr(ev("\"a${ 1 ~}  b\"", NULL), "a1b"));
  check("ws strip both", isstr(ev("\"x${~ 1 ~}y\"", NULL), "x1y"));
  check("ws strip directive", isstr(ev("\"  %{~ if true ~}X%{~ endif ~}  \"", NULL), "X"));
  /* heredoc for-loop with `~}` trimming real newlines -> "123" */
  check("ws strip heredoc for",
        isstr(ev("<<EOT\n%{ for n in [1,2,3] ~}\n${n}%{ endfor ~}\nEOT\n", NULL), "123"));
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
        "join(\",\", split(\";\", \"a;b;c\"))",
        "jsonencode({a = [1, true, \"s\"], b = null})",
        "jsondecode(\"[1,2,{\\\"k\\\":3}]\")",
        "concat([1], keys({a = 1}))",
        "coalesce(null, lookup({x = 5}, \"x\", 0))",
        "\"%{ if true }${1}%{ else }no%{ endif }\"",
        "\"%{ for n in [1, 2, 3] }${n},%{ endfor }\"",
        "<<EOF\nhi ${1 + 1}\nEOF\n",
        "<<-EOF\n  a\n    b\n  EOF\n",
        /* every builtin (drives each one's allocation/OOM arms) */
        "length(\"abc\")",
        "upper(\"a\")",
        "lower(\"A\")",
        "join(\",\", [\"a\", \"b\"])",
        "split(\",\", \"a,b,c\")",
        "split(\"\", \"hi\")",
        "abs(-2)",
        "floor(1.5)",
        "ceil(1.5)",
        "min(3, 1)",
        "max(3, 1)",
        "concat([1], [2], [3])",
        "keys({a = 1, b = 2})",
        "values({a = 1, b = 2})",
        "contains([1, 2], 2)",
        "lookup({a = 1}, \"a\", 0)",
        "coalesce(null, 5)",
        "tostring(42)",
        "tonumber(\"3.5\")",
        "tobool(\"true\")",
        "jsonencode({a = [1, true, \"x\\ny\"], b = null, c = -2.5})",
        "jsonencode(\"q\\\"\\\\\\u0001\")",
        "jsondecode(\"{\\\"k\\\": [1, 2, \\\"s\\\"]}\")",
        /* special forms, collection/template expressions */
        "try(nope, alsono, \"fb\")",
        "can(1 + 1)",
        "{for k, v in {a = 1, b = 2} : k => v if v > 0}",
        "[for x in [1, 2, 3] : x * 2 if x > 1]",
        "{for s in [\"a\", \"b\", \"a\"] : s => s...}",
        "[{a = [1, 2]}, {a = [3]}][*].a[0]",
        "{a = 1, b = {c = [2, 3]}}",
        "\"%{ for k, v in {a = 1} }${k}=${v} %{ endfor }\"",
        /* more node kinds, to drive their construction/OOM arms */
        "-5 + 2",
        "!false",
        "(1 + 2) * 3",
        "1 <= 2 && 3 >= 2",
        "true ? \"yes\" : \"no\"",
        "[{a = 1}].*.a",
        "[[1, 2], [3]][*][0]",
        "{a = 1}[\"a\"]",
        "<<-EOT\n  ${1 + 1}\n  line\n  EOT\n",
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

    /* multi-error parser: clean + malformed inputs drive the diagnostics-list
     * growth and recovery cleanup arms under every allocation budget. */
    const char *diagdocs[] = {
        "a = 1\nb = 2\n",
        "a = 1\nb = )\nc = 2\n* junk\n}\nsvc x {\n  bad = +\n}\nf = 4\n",
    };
    bool alldg = true;
    for (size_t i = 0; i < sizeof(diagdocs) / sizeof(diagdocs[0]); i++)
      alldg = oom_scan_diags(diagdocs[i]) && alldg;
    check("oom scan: diags", alldg);

    const char *jsons[] = {
        "{\"a\": 1, \"b\": [true, null, \"x\\u00e9\"], \"c\": {\"d\": -2.5e3}}",
        "[1, 2, 3]",
    };
    bool allj = true;
    for (size_t i = 0; i < sizeof(jsons) / sizeof(jsons[0]); i++)
      allj = oom_scan_json(jsons[i]) && allj;
    check("oom scan: json", allj);

    const char *jevals[] = {
        "{\"x\": \"v${a}\", \"y\": [1, \"${a}\"], \"z\": \"lit\"}",
        "\"%{ if true }${a}%{ endif }\"",
    };
    bool alje = true;
    for (size_t i = 0; i < sizeof(jevals) / sizeof(jevals[0]); i++)
      alje = oom_scan_jsoneval(jevals[i]) && alje;
    check("oom scan: json_eval", alje);

    /* schema-driven body decode: single block, array blocks, nested attrs */
    const char *jdecs[] = {
        "{\"name\": \"x-${a}\", \"tags\": [1, \"y\"], \"meta\": {\"k\": 1},"
        " \"service\": {\"api\": {\"port\": 8080, \"host\": \"h\"}}}",
        "{\"name\": \"x\", \"service\": {\"w\": [{\"port\": 80}, {\"port\": 443}]}}",
    };
    bool aljd = true;
    for (size_t i = 0; i < sizeof(jdecs) / sizeof(jdecs[0]); i++)
      aljd = oom_scan_json_decode(jdecs[i]) && aljd;
    check("oom scan: json_decode", aljd);

    /* convert() OOM paths: build inputs with the budget off, then fail each
       allocation inside the conversion until it succeeds. */
    hcl2_value *lsrc = ev("[1, \"2\", 3]", NULL);
    hcl2_value *msrc = ev("{a = \"1\", b = 2}", NULL);
    hcl2_value *nsrc = ev("42", NULL);
    hcl2_value *bsrc = ev("\"true\"", NULL);
    hcl2_value *usrc = hcl2_unknown(); /* drives type_clone + unknown_of */
    hcl2_type *lt = hcl2_type_set(hcl2_type_number());
    hcl2_type *mt = hcl2_type_map(hcl2_type_string());
    hcl2_type *st = hcl2_type_string(); /* singleton */
    hcl2_type *bt = hcl2_type_bool();   /* singleton */
    hcl2_type *at = hcl2_type_list(hcl2_type_any());
    hcl2_type *ut = hcl2_type_list(hcl2_type_map(hcl2_type_number())); /* nested clone */
    bool cok = false;
    for (int b = 0; b <= 5000; b++) {
      hcl2_alloc_budget = b;
      char e[64] = "";
      hcl2_value *o1 = hcl2_convert(lsrc, lt, e, sizeof(e));
      hcl2_value *o2 = hcl2_convert(msrc, mt, e, sizeof(e));
      hcl2_value *o3 = hcl2_convert(nsrc, st, e, sizeof(e));
      hcl2_value *o4 = hcl2_convert(bsrc, bt, e, sizeof(e));
      hcl2_value *o5 = hcl2_convert(lsrc, at, e, sizeof(e));
      hcl2_value *o6 = hcl2_convert(usrc, ut, e, sizeof(e)); /* unknown -> typed */
      hcl2_alloc_budget = -1;
      bool done = o1 && o2 && o3 && o4 && o5 && o6;
      hcl2_value_free(o1);
      hcl2_value_free(o2);
      hcl2_value_free(o3);
      hcl2_value_free(o4);
      hcl2_value_free(o5);
      hcl2_value_free(o6);
      if (done) {
        cok = true;
        break;
      }
    }
    check("oom scan: convert", cok);
    hcl2_value_free(lsrc);
    hcl2_value_free(msrc);
    hcl2_value_free(nsrc);
    hcl2_value_free(bsrc);
    hcl2_value_free(usrc);
    hcl2_type_free(lt);
    hcl2_type_free(mt);
    hcl2_type_free(at);
    hcl2_type_free(ut);

    /* ctx_set_var / ctx_set_func OOM (realloc + strdup-name arms) */
    bool ctxok = false;
    for (int b = 0; b <= 200; b++) {
      hcl2_alloc_budget = b;
      hcl2_ctx *cc = hcl2_ctx_new();
      bool done = false;
      if (cc != NULL) {
        hcl2_value *nv = hcl2_number(1);
        bool r1 = (nv != NULL) && hcl2_ctx_set_var(cc, "x", nv);
        if (!r1)
          hcl2_value_free(nv);
        bool r2 = hcl2_ctx_set_func(cc, "f", fn_inc);
        done = r1 && r2;
        hcl2_ctx_free(cc);
      }
      hcl2_alloc_budget = -1;
      if (done) {
        ctxok = true;
        break;
      }
    }
    check("oom scan: ctx", ctxok);
  }
#endif

  if (failures == 0) {
    fprintf(stderr, "\nAll c-hcl2 tests passed.\n");
    return 0;
  }
  fprintf(stderr, "\n%d c-hcl2 test(s) FAILED.\n", failures);
  return 1;
}

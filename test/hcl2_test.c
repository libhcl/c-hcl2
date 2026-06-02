/* Unit test for c-hcl2 (M1: expression engine). ASan:
 *   clang -I.. -O0 -g -fsanitize=address ../hcl2.c hcl2_test.c -lm -o hcl2_test && ./hcl2_test
 */
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

  if (failures == 0) {
    fprintf(stderr, "\nAll c-hcl2 tests passed.\n");
    return 0;
  }
  fprintf(stderr, "\n%d c-hcl2 test(s) FAILED.\n", failures);
  return 1;
}

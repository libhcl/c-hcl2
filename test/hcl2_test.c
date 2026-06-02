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

  if (failures == 0) {
    fprintf(stderr, "\nAll c-hcl2 tests passed.\n");
    return 0;
  }
  fprintf(stderr, "\n%d c-hcl2 test(s) FAILED.\n", failures);
  return 1;
}

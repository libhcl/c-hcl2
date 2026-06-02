#ifndef C_HCL2_INTERNAL_H
#define C_HCL2_INTERNAL_H

/* Private declarations shared between the c-hcl2 translation units
 * (value.c / lexer.c / parser.c / eval.c). Not part of the public API. */
#include "hcl2.h"

/* --- value model (value.c) --- */
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

hcl2_value *vclone(const hcl2_value *v);
bool vequal(const hcl2_value *a, const hcl2_value *b);
const hcl2_value *ctx_var(hcl2_ctx *c, const char *name);
hcl2_func ctx_func(hcl2_ctx *c, const char *name);
hcl2_func builtin_func(const char *name);

/* error reporter (defined in eval.c, used by the builtins in value.c too) */
void everr(char *err, size_t errsz, const char *m);

/* --- lexer (lexer.c) --- */
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
void lx_err(struct lexer *l, const char *m);
void lex(struct lexer *l);

/* --- AST + parser (parser.c) --- */
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
struct parser {
  struct lexer lx;
};
void node_free(struct node *x);
struct node *parse_expr(struct parser *p);

/* expression-node evaluator (eval.c), used by body attribute decoding */
hcl2_value *hcl2_eval_node(const struct node *x, hcl2_ctx *ctx, char *err, size_t errsz);

/* --- configuration bodies (body.c) --- */
struct hcl2_attr {
  char *name;
  struct node *expr; /* unevaluated; decoded lazily against a context */
};
struct hcl2_block {
  char *type;
  char **labels;
  size_t nlabel;
  struct hcl2_body *body;
};
struct hcl2_body {
  struct hcl2_attr **attrs;
  size_t nattr;
  struct hcl2_block **blocks;
  size_t nblock;
};
struct hcl2_doc {
  struct hcl2_body *root;
};
/* Parse a body. A top-level body ends at EOF; a nested one ends at '}' (left as
 * the current token, not consumed). Returns NULL on error. */
struct hcl2_body *parse_body(struct parser *p, bool toplevel);
void hcl2_body_free(struct hcl2_body *b);

#endif /* C_HCL2_INTERNAL_H */

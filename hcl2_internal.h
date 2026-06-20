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
  hcl2_type *utype; /* HCL2_UNKNOWN only: the cty type this unknown stands for
                     * (NULL == fully dynamic / "any"); owned by the value */
};

/* storage groups: item-indexed (tuple/list/set) vs string-keyed (object/map) */
static inline bool hcl2_is_seq(hcl2_kind k) {
  return k == HCL2_TUPLE || k == HCL2_LIST || k == HCL2_SET;
}
static inline bool hcl2_is_keyed(hcl2_kind k) { return k == HCL2_OBJECT || k == HCL2_MAP; }

hcl2_value *vclone(const hcl2_value *v);
bool vequal(const hcl2_value *a, const hcl2_value *b);
/* Deep-copy a type constraint (returns singletons as-is); defined in convert.c. */
hcl2_type *type_clone(const hcl2_type *t);
const hcl2_value *ctx_var(hcl2_ctx *c, const char *name);
/* Detach and return the owned value bound to name (NULL if absent); used to
 * save/restore for-expression loop-variable scope. */
hcl2_value *ctx_take_var(hcl2_ctx *c, const char *name);
hcl2_func ctx_func(hcl2_ctx *c, const char *name);
hcl2_func builtin_func(const char *name);

/* error reporter (defined in eval.c, used by the builtins in value.c too) */
void everr(char *err, size_t errsz, const char *m);

/* Evaluate a raw template string (defined in eval.c). With heredoc=true the
 * backslash escapes are kept literal and only ${ } / %{ } (and $${ / %%{) are
 * interpreted -- which is exactly the HCL JSON profile's template semantics,
 * since JSON has already processed its own escapes. Used by json.c. */
hcl2_value *eval_template(const char *raw, bool heredoc, hcl2_ctx *ctx, char *err, size_t errsz);

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
  T_FATARROW, /* => (object for-expression) */
  T_HEREDOC,  /* <<EOF / <<-EOF heredoc; text holds the raw (de-indented) body */
  T_ELLIPSIS, /* ... (variadic call-argument spread) */
};
struct lexer {
  const char *p, *end;
  const char *start;   /* source begin, for line/column diagnostics */
  const char *tokpos;  /* start of the current token, for error positions */
  const char *prevend; /* end of the previously consumed token (span ends) */
  enum tok tok;
  char *text; /* T_NUM/T_IDENT/T_STR (raw inner for strings); owned, reused */
  size_t tlen;
  char *err;
  size_t errsz;
};
void lx_err(struct lexer *l, const char *m);
void lex(struct lexer *l);
/* Line/column (1-based) of byte position `pos` within the source. */
void lx_linecol(const struct lexer *l, const char *pos, int *line, int *col);

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
  N_FOR_TUPLE,  /* [for v in coll : body if cond]  (splat desugars to this) */
  N_FOR_OBJECT, /* {for k, v in coll : kexpr => vexpr if cond} */
};
struct node {
  enum nkind kind;
  hcl2_value *lit; /* N_LIT */
  char *str;       /* N_TEMPLATE raw / N_VAR,N_ATTR,N_CALL name / N_FOR_* value var */
  char *kvar;      /* N_FOR_* : optional key/index variable name */
  enum tok op;     /* N_UNARY / N_BINARY */
  /* children. N_FOR_*: a=collection, b=body/key-expr, c=value-expr(object), d=cond */
  struct node *a, *b, *c, *d;
  struct node **items; /* N_TUPLE / N_CALL args / N_OBJECT vals */
  char **keys;         /* N_OBJECT keys */
  size_t n;
  int line, col;       /* source span start (1-based; 0 = unknown) */
  int endline, endcol; /* source span end, exclusive (1-based; 0 = unknown) */
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
 * the current token, not consumed). Returns NULL on error. When `dg` is
 * non-NULL, body-level errors are recorded into it and parsing recovers at the
 * next line instead of aborting (multi-error mode). */
struct hcl2_body *parse_body(struct parser *p, bool toplevel, struct hcl2_diags *dg);
void hcl2_body_free(struct hcl2_body *b);

#endif /* C_HCL2_INTERNAL_H */

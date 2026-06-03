#ifndef C_HCL2_HCL2_H
#define C_HCL2_HCL2_H

#include <stdbool.h>
#include <stddef.h>

/*
 * c-hcl2 -- a from-scratch C implementation of HCL2, the heavyweight companion
 * to libhcl/c-hcl (which parses only the declarative subset).
 *
 * STATUS: milestones 1-3 done, M4 in progress -- the HCL2 *expression*
 * language + value model, configuration *bodies* with lazy decoding, the
 * template & collection expressions (for, splat, heredocs, %{} directives,
 * variadic spread), line/column diagnostics, type constraints + conversion
 * (hcl2_type_* / hcl2_convert), and unknown values (hcl2_unknown). Not yet
 * spec-complete; see ROADMAP.md (the JSON profile's schema-driven body layer,
 * the distinct cty collection kinds and type-tracked unknowns, and full source
 * ranges are not done yet; the JSON *value* layer via hcl2_parse_json IS).
 *
 * Implemented now: numbers, booleans, null, quoted-string templates with
 * `${ expr }` interpolation, tuples `[...]`, objects `{ k = v, ... }`, unary
 * `- !`, binary arithmetic `+ - * / %`, comparison `== != < <= > >=`, logical
 * `&& ||`, the conditional `cond ? a : b`, parentheses, variable references
 * with `.attr` / `[index]` traversal, function calls against a context,
 * for-expressions (`[for x in xs : e]`, `{for k, v in m : k => v}`, with `if`
 * filters), splat (`xs[*].field`), heredocs (`<<EOF` / `<<-EOF`), template
 * directives (`%{ if }` / `%{ for }`) and variadic call spread (`f(xs...)`);
 * plus document bodies via hcl2_parse (see the "configuration bodies" section
 * below), with hash, line, and block comments.
 */

typedef enum {
  HCL2_NULL,
  HCL2_BOOL,
  HCL2_NUMBER,
  HCL2_STRING,
  HCL2_TUPLE,
  HCL2_OBJECT,
  HCL2_UNKNOWN, /* cty-style unknown: a placeholder whose concrete value is not
                 * yet known; operations on it propagate unknown */
} hcl2_kind;

typedef struct hcl2_value hcl2_value;
typedef struct hcl2_ctx hcl2_ctx;

/* --- value constructors (caller owns the result; free with hcl2_value_free) --- */
hcl2_value *hcl2_null(void);
hcl2_value *hcl2_unknown(void); /* a cty-style unknown placeholder */
hcl2_value *hcl2_bool(bool b);
hcl2_value *hcl2_number(double n);
hcl2_value *hcl2_string(const char *s);
hcl2_value *hcl2_tuple(void); /* empty; append with hcl2_tuple_push */
bool hcl2_tuple_push(hcl2_value *tuple, hcl2_value *elem /* owned */);
hcl2_value *hcl2_object(void); /* empty; set with hcl2_object_set */
bool hcl2_object_set(hcl2_value *object, const char *key, hcl2_value *val /* owned */);
void hcl2_value_free(hcl2_value *v);

/* --- value inspectors --- */
hcl2_kind hcl2_value_kind(const hcl2_value *v);
bool hcl2_value_is_unknown(const hcl2_value *v);
bool hcl2_value_as_bool(const hcl2_value *v, bool *out);
bool hcl2_value_as_number(const hcl2_value *v, double *out);
const char *hcl2_value_as_string(const hcl2_value *v);          /* NULL unless HCL2_STRING */
size_t hcl2_value_len(const hcl2_value *v);                     /* tuple/object size, else 0 */
const hcl2_value *hcl2_value_at(const hcl2_value *v, size_t i); /* tuple */
const hcl2_value *hcl2_value_get(const hcl2_value *v, const char *key); /* object */

/* --- type constraints & conversion (M4, in progress) ---
 *
 * A small cty-lite type model used as a *constraint*: hcl2_convert coerces a
 * value toward a target type (number<->string, string->bool, etc.) and
 * validates/normalises collections. NOTE: this value model has no distinct
 * list/set/map runtime kind yet -- list/set are represented as tuples and map
 * as an object -- so converting "to list(number)" yields a homogeneous tuple
 * (set additionally de-duplicates). The full cty type system (distinct
 * collection kinds, unknown values) is future M4 work; see ROADMAP.md.
 *
 *   hcl2_type *t = hcl2_type_list(hcl2_type_number());
 *   hcl2_value *nums = hcl2_convert(v, t, err, sizeof err);  // tuple of numbers
 *   hcl2_type_free(t);   // frees the list type and its owned element
 */
typedef struct hcl2_type hcl2_type;

/* Primitive/dynamic type singletons (do NOT free; hcl2_type_free is a no-op). */
hcl2_type *hcl2_type_any(void); /* dynamic: hcl2_convert is the identity */
hcl2_type *hcl2_type_bool(void);
hcl2_type *hcl2_type_number(void);
hcl2_type *hcl2_type_string(void);
/* Collection constructors; each TAKES OWNERSHIP of `elem` (free the result
 * with hcl2_type_free, which recurses into the element). */
hcl2_type *hcl2_type_list(hcl2_type *elem);
hcl2_type *hcl2_type_set(hcl2_type *elem);
hcl2_type *hcl2_type_map(hcl2_type *elem);
void hcl2_type_free(hcl2_type *t);

/* Convert v to type t. Returns a fresh owned value, or NULL on a type error
 * (message in err). */
hcl2_value *hcl2_convert(const hcl2_value *v, const hcl2_type *t, char *err, size_t errsz);

/* --- evaluation context --- */
hcl2_ctx *hcl2_ctx_new(void); /* pre-populated with the builtin functions */
void hcl2_ctx_free(hcl2_ctx *ctx);
/* Bind a variable. The context takes ownership of v. */
bool hcl2_ctx_set_var(hcl2_ctx *ctx, const char *name, hcl2_value *v);

/* A function receives evaluated arguments and returns a fresh owned value, or
 * NULL on error (writing into err). */
typedef hcl2_value *(*hcl2_func)(const hcl2_value *const *args, size_t n, char *err, size_t errsz);
bool hcl2_ctx_set_func(hcl2_ctx *ctx, const char *name, hcl2_func fn);

/* --- evaluation --- */
/* Parse and evaluate a single HCL2 expression. ctx may be NULL (no variables,
 * builtins only). Returns an owned value, or NULL on error (message in err). */
hcl2_value *hcl2_eval(const char *src, size_t len, hcl2_ctx *ctx, char *err, size_t errsz);

/* --- JSON profile (M5, in progress) ---
 * Parse a JSON document into the value model: object -> object, array -> tuple,
 * string -> string (literal, not yet an HCL template), number -> number,
 * true/false -> bool, null -> null. Returns an owned value, or NULL on error.
 * The schema-driven body profile (attribute vs. block, string templates) is
 * future work; see ROADMAP.md. */
hcl2_value *hcl2_parse_json(const char *src, size_t len, char *err, size_t errsz);

/* --- configuration bodies (M2) ---
 *
 * A document is a body of attributes (`name = expr`) and (optionally labeled)
 * blocks (`type "label" { ... }`). Parse once into an immutable tree, walk it
 * with the accessors below, then free. Attribute *expressions* are kept
 * unevaluated and decoded lazily, against a caller context, via
 * hcl2_body_attr_value -- so the same document can be evaluated against
 * different variable bindings.
 *
 *   hcl2_doc *doc = hcl2_parse(src, len, err, sizeof err);
 *   const hcl2_body *root = hcl2_doc_root(doc);
 *   hcl2_value *port = hcl2_body_attr_value(root, "port", ctx, err, sizeof err);
 *   ... hcl2_value_free(port); ...
 *   hcl2_doc_free(doc);
 */
typedef struct hcl2_doc hcl2_doc;
typedef struct hcl2_body hcl2_body;
typedef struct hcl2_block hcl2_block;

/* Parse a whole document body. Returns NULL on error (message in err). */
hcl2_doc *hcl2_parse(const char *src, size_t len, char *err, size_t errsz);
void hcl2_doc_free(hcl2_doc *doc);
const hcl2_body *hcl2_doc_root(const hcl2_doc *doc);

/* --- body: attributes --- */
size_t hcl2_body_attr_count(const hcl2_body *b);
const char *hcl2_body_attr_name(const hcl2_body *b, size_t i); /* i-th attr name, or NULL */
bool hcl2_body_has_attr(const hcl2_body *b, const char *name);
/* Evaluate the named attribute's expression against ctx (which may be NULL).
 * Returns an owned value, or NULL if absent or on evaluation error (err set). */
hcl2_value *hcl2_body_attr_value(const hcl2_body *b, const char *name, hcl2_ctx *ctx, char *err,
                                 size_t errsz);

/* --- body: blocks --- (type == NULL matches blocks of every type) */
size_t hcl2_body_block_count(const hcl2_body *b, const char *type);
const hcl2_block *hcl2_body_block_at(const hcl2_body *b, const char *type, size_t idx);
const char *hcl2_block_type(const hcl2_block *bl);
size_t hcl2_block_label_count(const hcl2_block *bl);
const char *hcl2_block_label(const hcl2_block *bl, size_t i);
const hcl2_body *hcl2_block_body(const hcl2_block *bl);

#endif /* C_HCL2_HCL2_H */

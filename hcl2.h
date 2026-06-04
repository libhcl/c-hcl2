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
 * (hcl2_type_* / hcl2_convert), the distinct cty collection kinds (list/set/
 * map), and unknown values (hcl2_unknown). Not yet spec-complete; see
 * ROADMAP.md (the JSON profile's schema-driven body layer, type-tracked
 * unknowns, big-number precision, and full source ranges are not done yet; the
 * JSON *value* layer via hcl2_parse_json IS).
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
  /* structural collections (heterogeneous, fixed shape) */
  HCL2_TUPLE,  /* ordered, item-indexed   */
  HCL2_OBJECT, /* string-keyed            */
  /* cty collection types (homogeneous element type) */
  HCL2_LIST,    /* ordered, item-indexed; same storage as a tuple   */
  HCL2_SET,     /* unordered, de-duplicated; same storage as a tuple */
  HCL2_MAP,     /* string-keyed; same storage as an object           */
  HCL2_UNKNOWN, /* cty-style unknown: a placeholder whose concrete value is not
                 * yet known; operations on it propagate unknown */
} hcl2_kind;

typedef struct hcl2_value hcl2_value;
typedef struct hcl2_ctx hcl2_ctx;
typedef struct hcl2_type hcl2_type;

/* --- value constructors (caller owns the result; free with hcl2_value_free) --- */
hcl2_value *hcl2_null(void);
hcl2_value *hcl2_unknown(void); /* a cty-style unknown placeholder (dynamic type) */
/* A typed unknown: a placeholder known to be of type `type` (TAKES OWNERSHIP of
 * `type`, like the collection type constructors). hcl2_convert refines an
 * unknown to its target type, so a converted unknown reports that type. */
hcl2_value *hcl2_unknown_of(hcl2_type *type);
hcl2_value *hcl2_bool(bool b);
hcl2_value *hcl2_number(double n);
hcl2_value *hcl2_string(const char *s);
hcl2_value *hcl2_tuple(void); /* empty; append with hcl2_tuple_push */
hcl2_value *hcl2_list(void);  /* empty cty list; append with hcl2_tuple_push */
hcl2_value *hcl2_set(void);   /* empty cty set;  append with hcl2_tuple_push */
/* Append to a tuple, list or set (any item-indexed collection). */
bool hcl2_tuple_push(hcl2_value *seq, hcl2_value *elem /* owned */);
hcl2_value *hcl2_object(void); /* empty; set with hcl2_object_set */
hcl2_value *hcl2_map(void);    /* empty cty map; set with hcl2_object_set */
/* Set a key on an object or map (any string-keyed collection). */
bool hcl2_object_set(hcl2_value *obj, const char *key, hcl2_value *val /* owned */);
void hcl2_value_free(hcl2_value *v);

/* --- value inspectors --- */
hcl2_kind hcl2_value_kind(const hcl2_value *v);
bool hcl2_value_is_unknown(const hcl2_value *v);
/* The cty type an unknown stands for: a typed unknown returns its type, a
 * dynamic unknown (hcl2_unknown) returns hcl2_type_any(), and a non-unknown
 * value returns NULL. The returned type is owned by the value -- do not free. */
const hcl2_type *hcl2_unknown_type(const hcl2_value *v);
bool hcl2_value_as_bool(const hcl2_value *v, bool *out);
bool hcl2_value_as_number(const hcl2_value *v, double *out);
const char *hcl2_value_as_string(const hcl2_value *v); /* NULL unless HCL2_STRING */
size_t hcl2_value_len(const hcl2_value *v);            /* tuple/list/set/object/map size, else 0 */
const hcl2_value *hcl2_value_at(const hcl2_value *v, size_t i);         /* tuple/list/set */
const hcl2_value *hcl2_value_get(const hcl2_value *v, const char *key); /* object/map */

/* --- type constraints & conversion (M4, in progress) ---
 *
 * A small cty type model used as a *constraint*: hcl2_convert coerces a value
 * toward a target type (number<->string, string->bool, etc.) and produces the
 * matching cty collection kind. Converting to list(T)/set(T)/map(T) yields an
 * HCL2_LIST / HCL2_SET / HCL2_MAP whose elements are each converted to T (a set
 * additionally de-duplicates). A tuple converts to a list/set, an object to a
 * map. (Still future: type-tracked unknowns and big-number precision.)
 *
 *   hcl2_type *t = hcl2_type_list(hcl2_type_number());
 *   hcl2_value *nums = hcl2_convert(v, t, err, sizeof err);  // tuple of numbers
 *   hcl2_type_free(t);   // frees the list type and its owned element
 */

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
 * string -> string (literal, NOT an HCL template), number -> number,
 * true/false -> bool, null -> null. Returns an owned value, or NULL on error. */
hcl2_value *hcl2_parse_json(const char *src, size_t len, char *err, size_t errsz);

/* Evaluate an HCL JSON-profile document. Like hcl2_parse_json, but each JSON
 * string is interpreted as an HCL template and evaluated against `ctx`, so
 * "${var}" / "%{ if c }..%{ endif }" expand (backslashes stay literal -- JSON
 * already did its own un-escaping). Objects -> object, arrays -> tuple, and
 * numbers/bools/null map directly. Returns an owned value, or NULL on error.
 * (This is the JSON profile's expression decoding; the schema-driven body
 * profile -- attribute-vs-block resolution -- is still future work, see
 * ROADMAP.md.) */
hcl2_value *hcl2_json_eval(const char *src, size_t len, hcl2_ctx *ctx, char *err, size_t errsz);

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

/* --- multi-error parsing (M4) ---
 * Like hcl2_parse, but instead of stopping at the first error it recovers at
 * the next line and collects *all* body-level errors into a diagnostics list.
 * Returns a best-effort (possibly partial) document, or NULL if nothing could
 * be parsed; *out always receives a diagnostics list to free with
 * hcl2_diags_free. A successful parse yields hcl2_diags_count() == 0.
 *
 *   hcl2_diags *d = NULL;
 *   hcl2_doc *doc = hcl2_parse_diags(src, len, &d);
 *   for (size_t i = 0; i < hcl2_diags_count(d); i++)
 *     fprintf(stderr, "%s\n", hcl2_diags_msg(d, i));
 *   hcl2_diags_free(d); hcl2_doc_free(doc);
 */
typedef struct hcl2_diags hcl2_diags;
hcl2_doc *hcl2_parse_diags(const char *src, size_t len, hcl2_diags **out);
size_t hcl2_diags_count(const hcl2_diags *d);
const char *hcl2_diags_msg(const hcl2_diags *d, size_t i); /* "hcl2: ... at line L, column C" */
void hcl2_diags_free(hcl2_diags *d);
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

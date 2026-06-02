#ifndef C_HCL2_HCL2_H
#define C_HCL2_HCL2_H

#include <stdbool.h>
#include <stddef.h>

/*
 * c-hcl2 -- a from-scratch C implementation of HCL2, the heavyweight companion
 * to libhcl/c-hcl (which parses only the declarative subset).
 *
 * STATUS: milestone 1 -- the HCL2 *expression* language and value model. This
 * is the part that distinguishes HCL2 from a plain config subset. Not yet
 * spec-complete; see ROADMAP.md (heredocs, for-expressions, splat, %{}
 * template directives, the JSON profile, the full cty type system with unknown
 * values, and source-range diagnostics are not done yet).
 *
 * Implemented now: numbers, booleans, null, quoted-string templates with
 * `${ expr }` interpolation, tuples `[...]`, objects `{ k = v, ... }`, unary
 * `- !`, binary arithmetic `+ - * / %`, comparison `== != < <= > >=`, logical
 * `&& ||`, the conditional `cond ? a : b`, parentheses, variable references
 * with `.attr` / `[index]` traversal, and function calls against a context.
 */

typedef enum {
  HCL2_NULL,
  HCL2_BOOL,
  HCL2_NUMBER,
  HCL2_STRING,
  HCL2_TUPLE,
  HCL2_OBJECT,
} hcl2_kind;

typedef struct hcl2_value hcl2_value;
typedef struct hcl2_ctx hcl2_ctx;

/* --- value constructors (caller owns the result; free with hcl2_value_free) --- */
hcl2_value *hcl2_null(void);
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
bool hcl2_value_as_bool(const hcl2_value *v, bool *out);
bool hcl2_value_as_number(const hcl2_value *v, double *out);
const char *hcl2_value_as_string(const hcl2_value *v);          /* NULL unless HCL2_STRING */
size_t hcl2_value_len(const hcl2_value *v);                     /* tuple/object size, else 0 */
const hcl2_value *hcl2_value_at(const hcl2_value *v, size_t i); /* tuple */
const hcl2_value *hcl2_value_get(const hcl2_value *v, const char *key); /* object */

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

#endif /* C_HCL2_HCL2_H */

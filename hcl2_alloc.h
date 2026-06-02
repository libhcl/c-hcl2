#ifndef C_HCL2_ALLOC_H
#define C_HCL2_ALLOC_H

/* Test-only allocation fault injection, shared by every c-hcl2 translation
 * unit. With HCL2_FAULT_INJECT defined (test/cover builds only), set
 * hcl2_alloc_budget >= 0 and the (budget+1)-th allocation fails once, then the
 * hook disables itself. Used to drive out-of-memory branches deterministically.
 * The storage for hcl2_alloc_budget is defined in value.c. */
#ifdef HCL2_FAULT_INJECT
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

extern int hcl2_alloc_budget;

static inline bool hcl2__should_fail(void) {
  if (hcl2_alloc_budget < 0)
    return false;
  if (hcl2_alloc_budget == 0) {
    hcl2_alloc_budget = -1;
    return true;
  }
  hcl2_alloc_budget--;
  return false;
}
static inline void *hcl2__malloc(size_t n) { return hcl2__should_fail() ? NULL : malloc(n); }
static inline void *hcl2__calloc(size_t a, size_t b) {
  return hcl2__should_fail() ? NULL : calloc(a, b);
}
static inline void *hcl2__realloc(void *p, size_t n) {
  return hcl2__should_fail() ? NULL : realloc(p, n);
}
static inline char *hcl2__strdup(const char *s) { return hcl2__should_fail() ? NULL : strdup(s); }
#define malloc hcl2__malloc
#define calloc hcl2__calloc
#define realloc hcl2__realloc
#define strdup hcl2__strdup
#endif /* HCL2_FAULT_INJECT */

#endif /* C_HCL2_ALLOC_H */

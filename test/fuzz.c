/* Deterministic fuzzer for the c-hcl2 lexer/parser/evaluator. It throws random,
 * token-soup, and mutated inputs at hcl2_eval and hcl2_parse; the contract is
 * simply that neither ever crashes, leaks, or reads out of bounds on any input.
 * Run it under AddressSanitizer to make that meaningful:
 *
 *   make fuzz SANITIZE=address                 # default 200k iterations
 *   ./test/fuzz 5000000 0xC0FFEE                # iterations + PRNG seed
 *
 * The input buffer is allocated at the *exact* length (no NUL terminator) so
 * ASan flags any read past the end -- hcl2_* must honour (src, len) strictly.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"

static uint32_t S = 0x9E3779B9u;
static uint32_t rnd(void) {
  S ^= S << 13;
  S ^= S >> 17;
  S ^= S << 5;
  return S;
}
static uint32_t rndn(uint32_t n) { return n ? rnd() % n : 0; }

/* Bytes biased toward HCL2 syntax so random input still reaches deep paths. */
static const char CHARSET[] = "{}[]()=><!&|+-*/%?:,.\"\\$# \t\n_abcxyz0129"
                              "fortuelsnifTRUEN";
static const char *const SEEDS[] = {
    "1 + 2 * 3",
    "[for x, y in [1, 2, 3] : x + y if y > 1]",
    "{for k, v in {a = 1, b = 2} : k => v}",
    "\"a${1 + 1}b %{ if true }c%{ else }d%{ endif }\"",
    "\"%{ for n in [1, 2, 3] }${n},%{ endfor }\"",
    "<<-EOT\n  hello ${1}\n    world\n  EOT\n",
    "max([1, 2, 3]...)",
    "items[*].name",
    "name = \"x\"\nport = 1 + 1\nsvc \"a\" \"b\" { v = [1, 2] inner { z = 3 } }\n",
    "cond ? {a = 1} : [1, 2, 3]",
};
#define NSEEDS ((int)(sizeof(SEEDS) / sizeof(SEEDS[0])))

/* Build one fuzz input into a freshly-sized heap buffer; returns its length. */
static char *gen(size_t *outlen) {
  uint32_t mode = rndn(3);
  size_t len;
  char tmp[256];
  if (mode == 0) { /* random bytes from the biased charset */
    len = rndn(64);
    for (size_t i = 0; i < len; i++)
      tmp[i] = CHARSET[rndn((uint32_t)(sizeof(CHARSET) - 1))];
  } else if (mode == 1) { /* token soup */
    static const char *toks[] = {
        "for",    "in",      "if",    "true", "false", "null", "[",  "]",   "{",  "}",  "(",
        ")",      "=",       "=>",    ":",    "?",     "+",    "*",  "...", "${", "%{", "endif",
        "endfor", "<<EOF\n", "EOF\n", "\"",   "1.5",   "x",    ".y", "1e9", ",",  " "};
    len = 0;
    int n = (int)rndn(12) + 1;
    for (int t = 0; t < n; t++) {
      const char *tk = toks[rndn((uint32_t)(sizeof(toks) / sizeof(toks[0])))];
      size_t tl = strlen(tk);
      if (len + tl >= sizeof(tmp))
        break;
      memcpy(tmp + len, tk, tl);
      len += tl;
    }
  } else { /* mutate a valid seed */
    const char *seed = SEEDS[rndn(NSEEDS)];
    len = strlen(seed);
    if (len >= sizeof(tmp))
      len = sizeof(tmp) - 1;
    memcpy(tmp, seed, len);
    int muts = (int)rndn(6) + 1;
    for (int m = 0; m < muts && len > 0; m++) {
      uint32_t op = rndn(3), at = rndn((uint32_t)len);
      if (op == 0) { /* replace */
        tmp[at] = CHARSET[rndn((uint32_t)(sizeof(CHARSET) - 1))];
      } else if (op == 1 && len + 1 < sizeof(tmp)) { /* insert */
        memmove(tmp + at + 1, tmp + at, len - at);
        tmp[at] = CHARSET[rndn((uint32_t)(sizeof(CHARSET) - 1))];
        len++;
      } else { /* delete */
        memmove(tmp + at, tmp + at + 1, len - at - 1);
        len--;
      }
    }
  }
  /* exact-size copy: no NUL, so ASan catches any over-read */
  char *buf = malloc(len ? len : 1);
  if (buf && len)
    memcpy(buf, tmp, len);
  *outlen = len;
  return buf;
}

int main(int argc, char **argv) {
  long iters = (argc > 1) ? strtol(argv[1], NULL, 0) : 200000;
  if (argc > 2)
    S = (uint32_t)strtoul(argv[2], NULL, 0);
  if (S == 0)
    S = 1;

  for (long i = 0; i < iters; i++) {
    size_t len;
    char *buf = gen(&len);
    if (buf == NULL)
      continue;
    char err[256];

    hcl2_value *v = hcl2_eval(buf, len, NULL, err, sizeof(err));
    hcl2_value_free(v);

    hcl2_doc *d = hcl2_parse(buf, len, err, sizeof(err));
    hcl2_doc_free(d);

    hcl2_value *jv = hcl2_parse_json(buf, len, err, sizeof(err));
    hcl2_value_free(jv);

    free(buf);
  }
  fprintf(stderr, "fuzz: %ld iterations, seed end-state 0x%08x, no crash.\n", iters, S);
  return 0;
}

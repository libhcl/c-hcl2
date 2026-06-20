#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* A small RE2-subset regular-expression engine (no external dependency),
 * sufficient for Terraform's regex/regexall/replace. It supports literals,
 * '.', character classes [..] (ranges, negation, \d \w \s and uppercase
 * complements), anchors ^ $, word boundaries \b \B, alternation |, groups
 * (capturing, (?:...), and named (?P<n>...) / (?<n>...)), and the quantifiers
 * * + ? {n} {n,} {n,m} in greedy and lazy (trailing ?) forms. Matching is
 * leftmost (Perl/Go default), via a backtracking byte-code VM with a step cap.
 *
 * Backreferences and look-around are intentionally absent (RE2 has neither). */

/* ---- bytecode ---- */
enum { I_CHAR, I_ANY, I_CLASS, I_MATCH, I_JMP, I_SPLIT, I_SAVE, I_BOL, I_EOL, I_WB, I_NWB };
typedef struct {
  int op;
  unsigned char c;    /* I_CHAR */
  int x, y;           /* I_JMP/I_SPLIT targets; I_SAVE slot in x */
  unsigned char *cls; /* I_CLASS: 32-byte (256-bit) membership bitmap */
} inst;

struct hregex {
  inst *p;
  int n, cap;
  int ngroup;      /* number of capture groups (excluding whole-match) */
  char *names[64]; /* names[g] = name of group g (1-based), or NULL */
};

/* ---- parser building bytecode directly (with backpatching) ---- */
typedef struct {
  const char *p, *end;
  struct hregex *re;
  int ncap;
  char *err;
  size_t errsz;
  bool ok;
} pstate;

static int emit(struct hregex *re, inst in) {
  if (re->n == re->cap) {
    int nc = re->cap ? re->cap * 2 : 32;
    inst *np = realloc(re->p, (size_t)nc * sizeof *np);
    if (np == NULL)
      return -1;
    re->p = np;
    re->cap = nc;
  }
  re->p[re->n] = in;
  return re->n++;
}

static void perr(pstate *s, const char *m) {
  if (s->ok && s->err)
    snprintf(s->err, s->errsz, "regex: %s", m);
  s->ok = false;
}

static void cls_set(unsigned char *b, unsigned c) { b[c >> 3] |= (unsigned char)(1u << (c & 7)); }
static void cls_range(unsigned char *b, unsigned lo, unsigned hi) {
  for (unsigned c = lo; c <= hi; c++)
    cls_set(b, c);
}
static void cls_named(unsigned char *b, char kind) {
  switch (kind) {
  case 'd':
    cls_range(b, '0', '9');
    break;
  case 'w':
    cls_range(b, '0', '9');
    cls_range(b, 'A', 'Z');
    cls_range(b, 'a', 'z');
    cls_set(b, '_');
    break;
  case 's':
    cls_set(b, ' ');
    cls_set(b, '\t');
    cls_set(b, '\n');
    cls_set(b, '\r');
    cls_set(b, '\f');
    cls_set(b, '\v');
    break;
  }
}

static void parse_alt(pstate *s);

/* parse a [...] class, leaving *s->p past the closing ']' */
static int parse_class(pstate *s) {
  unsigned char *b = calloc(32, 1);
  if (b == NULL) {
    perr(s, "out of memory");
    return -1;
  }
  bool neg = false;
  if (s->p < s->end && *s->p == '^') {
    neg = true;
    s->p++;
  }
  bool first = true;
  while (s->p < s->end && (*s->p != ']' || first)) {
    unsigned lo;
    if (*s->p == '\\' && s->p + 1 < s->end) {
      char k = s->p[1];
      if (k == 'd' || k == 'w' || k == 's') {
        cls_named(b, k);
        s->p += 2;
        first = false;
        continue;
      }
      if (k == 'D' || k == 'W' || k == 'S') {
        unsigned char t[32] = {0};
        cls_named(t, (char)tolower((unsigned char)k));
        for (int i = 0; i < 32; i++)
          b[i] |= (unsigned char)~t[i];
        s->p += 2;
        first = false;
        continue;
      }
      lo = (unsigned char)(k == 'n' ? '\n' : k == 't' ? '\t' : k == 'r' ? '\r' : (unsigned char)k);
      s->p += 2;
    } else {
      lo = (unsigned char)*s->p++;
    }
    first = false;
    if (s->p + 1 < s->end && *s->p == '-' && s->p[1] != ']') {
      unsigned hi = (unsigned char)s->p[1];
      s->p += 2;
      cls_range(b, lo, hi);
    } else {
      cls_set(b, lo);
    }
  }
  if (s->p >= s->end || *s->p != ']') {
    perr(s, "unterminated [ ]");
    free(b);
    return -1;
  }
  s->p++;
  if (neg)
    for (int i = 0; i < 32; i++)
      b[i] = (unsigned char)~b[i];
  inst in = {.op = I_CLASS, .cls = b};
  return emit(s->re, in);
}

/* parse one atom; returns the pc of its first instruction (-1 on error/empty) */
static int parse_atom(pstate *s, bool *empty) {
  *empty = false;
  if (s->p >= s->end)
    return -1;
  char c = *s->p;
  if (c == '(') {
    s->p++;
    int capidx = -1;
    char *name = NULL;
    if (s->p < s->end && *s->p == '?') {
      s->p++;
      if (s->p < s->end && *s->p == ':') {
        s->p++; /* non-capturing */
      } else if (s->p < s->end && (*s->p == 'P' || *s->p == '<')) {
        if (*s->p == 'P')
          s->p++;
        if (s->p >= s->end || *s->p != '<') {
          perr(s, "bad named group");
          return -1;
        }
        s->p++;
        const char *ns = s->p;
        while (s->p < s->end && *s->p != '>')
          s->p++;
        if (s->p >= s->end) {
          perr(s, "unterminated group name");
          return -1;
        }
        name = strndup(ns, (size_t)(s->p - ns));
        s->p++;
        capidx = ++s->ncap;
      } else {
        perr(s, "unsupported (?...) group");
        return -1;
      }
    } else {
      capidx = ++s->ncap;
    }
    int save0 = -1;
    if (capidx >= 0) {
      if (capidx < 64)
        s->re->names[capidx] = name;
      else
        free(name);
      inst sv = {.op = I_SAVE, .x = 2 * capidx};
      save0 = emit(s->re, sv);
    } else {
      free(name);
    }
    int start = (save0 >= 0) ? save0 : s->re->n;
    parse_alt(s);
    if (capidx >= 0) {
      inst sv = {.op = I_SAVE, .x = 2 * capidx + 1};
      emit(s->re, sv);
    }
    if (s->p >= s->end || *s->p != ')') {
      perr(s, "missing )");
      return -1;
    }
    s->p++;
    return start;
  }
  if (c == '[')
    return s->p++, parse_class(s);
  if (c == '.')
    return s->p++, emit(s->re, (inst){.op = I_ANY});
  if (c == '^')
    return s->p++, emit(s->re, (inst){.op = I_BOL});
  if (c == '$')
    return s->p++, emit(s->re, (inst){.op = I_EOL});
  if (c == '\\' && s->p + 1 < s->end) {
    char k = s->p[1];
    s->p += 2;
    if (k == 'd' || k == 'w' || k == 's' || k == 'D' || k == 'W' || k == 'S') {
      unsigned char *b = calloc(32, 1);
      if (b == NULL) {
        perr(s, "out of memory");
        return -1;
      }
      char lk = (char)tolower((unsigned char)k);
      cls_named(b, lk);
      if (k >= 'A' && k <= 'Z')
        for (int i = 0; i < 32; i++)
          b[i] = (unsigned char)~b[i];
      return emit(s->re, (inst){.op = I_CLASS, .cls = b});
    }
    if (k == 'b')
      return emit(s->re, (inst){.op = I_WB});
    if (k == 'B')
      return emit(s->re, (inst){.op = I_NWB});
    unsigned char lit = (unsigned char)(k == 'n'   ? '\n'
                                        : k == 't' ? '\t'
                                        : k == 'r' ? '\r'
                                                   : (unsigned char)k);
    return emit(s->re, (inst){.op = I_CHAR, .c = lit});
  }
  if (c == ')' || c == '|') {
    *empty = true;
    return -1; /* not an atom here */
  }
  s->p++;
  return emit(s->re, (inst){.op = I_CHAR, .c = (unsigned char)c});
}

/* wrap [astart,aend) with a quantifier */
static void apply_quant(pstate *s, int astart, char q, bool lazy) {
  struct hregex *re = s->re;
  if (q == '*') {
    /* L1: split(body, out); body...; jmp L1; out: -- relocate body */
    int len = re->n - astart;
    inst *tmp = malloc((size_t)len * sizeof *tmp);
    if (tmp == NULL) {
      perr(s, "out of memory");
      return;
    }
    memcpy(tmp, re->p + astart, (size_t)len * sizeof *tmp);
    re->n = astart;
    int l1 = emit(re, (inst){.op = I_SPLIT});
    for (int i = 0; i < len; i++)
      emit(re, tmp[i]);
    emit(re, (inst){.op = I_JMP, .x = l1});
    int out = re->n;
    re->p[l1].x = lazy ? out : l1 + 1;
    re->p[l1].y = lazy ? l1 + 1 : out;
    /* fix relative JMP/SPLIT targets inside the moved body */
    for (int i = l1 + 1; i < l1 + 1 + len; i++)
      if (re->p[i].op == I_JMP || re->p[i].op == I_SPLIT) {
        if (re->p[i].x >= astart)
          re->p[i].x += (l1 + 1 - astart);
        if (re->p[i].op == I_SPLIT && re->p[i].y >= astart)
          re->p[i].y += (l1 + 1 - astart);
      }
    free(tmp);
  } else if (q == '+') {
    int l3 = emit(re, (inst){.op = I_SPLIT});
    re->p[l3].x = lazy ? re->n : astart;
    re->p[l3].y = lazy ? astart : re->n;
  } else if (q == '?') {
    int len = re->n - astart;
    inst *tmp = malloc((size_t)len * sizeof *tmp);
    if (tmp == NULL) {
      perr(s, "out of memory");
      return;
    }
    memcpy(tmp, re->p + astart, (size_t)len * sizeof *tmp);
    re->n = astart;
    int sp = emit(re, (inst){.op = I_SPLIT});
    for (int i = 0; i < len; i++)
      emit(re, tmp[i]);
    int out = re->n;
    re->p[sp].x = lazy ? out : sp + 1;
    re->p[sp].y = lazy ? sp + 1 : out;
    for (int i = sp + 1; i < sp + 1 + len; i++)
      if (re->p[i].op == I_JMP || re->p[i].op == I_SPLIT) {
        if (re->p[i].x >= astart)
          re->p[i].x += (sp + 1 - astart);
        if (re->p[i].op == I_SPLIT && re->p[i].y >= astart)
          re->p[i].y += (sp + 1 - astart);
      }
    free(tmp);
  }
}

static void parse_repeat(pstate *s) {
  bool empty;
  int astart = s->re->n;
  int first = parse_atom(s, &empty);
  if (!s->ok || empty || first < 0)
    return;
  while (s->p < s->end && (*s->p == '*' || *s->p == '+' || *s->p == '?' || *s->p == '{')) {
    char q = *s->p;
    if (q == '{') {
      /* {n}, {n,}, {n,m}: expand by copying the atom's bytecode */
      const char *save = s->p;
      s->p++;
      int lo = 0, hi = -1;
      bool has_lo = false, comma = false;
      while (s->p < s->end && isdigit((unsigned char)*s->p)) {
        lo = lo * 10 + (*s->p++ - '0');
        has_lo = true;
      }
      if (s->p < s->end && *s->p == ',') {
        comma = true;
        s->p++;
        if (s->p < s->end && isdigit((unsigned char)*s->p)) {
          hi = 0;
          while (s->p < s->end && isdigit((unsigned char)*s->p))
            hi = hi * 10 + (*s->p++ - '0');
        }
      }
      if (s->p >= s->end || *s->p != '}' || !has_lo) {
        s->p = save; /* not a valid quantifier: treat '{' literally */
        break;
      }
      s->p++;
      if (!comma)
        hi = lo;
      bool lazy = (s->p < s->end && *s->p == '?');
      if (lazy)
        s->p++;
      int len = s->re->n - astart;
      inst *body = malloc((size_t)(len ? len : 1) * sizeof *body);
      if (body == NULL) {
        perr(s, "out of memory");
        return;
      }
      memcpy(body, s->re->p + astart, (size_t)len * sizeof *body);
      s->re->n = astart; /* rebuild from scratch */
      int base = astart;
      for (int r = 0; r < lo; r++) {
        int here = s->re->n;
        for (int i = 0; i < len; i++) {
          inst in = body[i];
          if ((in.op == I_JMP || in.op == I_SPLIT)) {
            if (in.x >= base)
              in.x += here - base;
            if (in.op == I_SPLIT && in.y >= base)
              in.y += here - base;
          }
          emit(s->re, in);
        }
        base = here;
      }
      if (comma && hi < 0) {
        /* {n,}: a final greedy/lazy star on one more copy */
        int cs = s->re->n;
        for (int i = 0; i < len; i++) {
          inst in = body[i];
          if ((in.op == I_JMP || in.op == I_SPLIT)) {
            if (in.x >= base)
              in.x += cs - base;
            if (in.op == I_SPLIT && in.y >= base)
              in.y += cs - base;
          }
          emit(s->re, in);
        }
        apply_quant(s, cs, '*', lazy);
      } else {
        for (int r = lo; r < hi; r++) {
          int cs = s->re->n;
          int b2 = (r == lo) ? astart : cs; /* targets in body are relative to its own copy */
          for (int i = 0; i < len; i++) {
            inst in = body[i];
            if ((in.op == I_JMP || in.op == I_SPLIT)) {
              if (in.x >= b2)
                in.x += cs - b2;
              if (in.op == I_SPLIT && in.y >= b2)
                in.y += cs - b2;
            }
            emit(s->re, in);
          }
          apply_quant(s, cs, '?', lazy);
        }
      }
      free(body);
      continue;
    }
    s->p++;
    bool lazy = (s->p < s->end && *s->p == '?');
    if (lazy)
      s->p++;
    apply_quant(s, astart, q, lazy);
  }
}

static void parse_concat(pstate *s) {
  while (s->p < s->end && *s->p != '|' && *s->p != ')') {
    int before = s->re->n;
    parse_repeat(s);
    if (!s->ok)
      return;
    if (s->re->n == before)
      break; /* made no progress (empty atom) */
  }
}

static void parse_alt(pstate *s) {
  int start = s->re->n;
  parse_concat(s);
  if (!s->ok)
    return;
  if (s->p < s->end && *s->p == '|') {
    /* relocate the left side after a SPLIT, add a JMP at its end to skip right */
    int len = s->re->n - start;
    inst *left = malloc((size_t)(len ? len : 1) * sizeof *left);
    if (left == NULL) {
      perr(s, "out of memory");
      return;
    }
    memcpy(left, s->re->p + start, (size_t)len * sizeof *left);
    s->re->n = start;
    int sp = emit(s->re, (inst){.op = I_SPLIT});
    int shift = (sp + 1) - start;
    for (int i = 0; i < len; i++) {
      inst in = left[i];
      if (in.op == I_JMP || in.op == I_SPLIT) {
        if (in.x >= start)
          in.x += shift;
        if (in.op == I_SPLIT && in.y >= start)
          in.y += shift;
      }
      emit(s->re, in);
    }
    free(left);
    int jmp = emit(s->re, (inst){.op = I_JMP});
    s->re->p[sp].x = sp + 1;
    s->re->p[sp].y = s->re->n;
    s->p++; /* consume '|' */
    parse_alt(s);
    s->re->p[jmp].x = s->re->n;
  }
}

hregex *hre_compile(const char *pat, char *err, size_t errsz) {
  struct hregex *re = calloc(1, sizeof *re);
  if (re == NULL) {
    if (err)
      snprintf(err, errsz, "regex: out of memory");
    return NULL;
  }
  pstate s = {.p = pat, .end = pat + strlen(pat), .re = re, .err = err, .errsz = errsz, .ok = true};
  /* whole-match save 0 ... 1 */
  emit(re, (inst){.op = I_SAVE, .x = 0});
  parse_alt(&s);
  emit(re, (inst){.op = I_SAVE, .x = 1});
  emit(re, (inst){.op = I_MATCH});
  re->ngroup = s.ncap;
  if (!s.ok || (s.p != s.end)) {
    if (s.ok && err)
      snprintf(err, errsz, "regex: trailing characters in pattern");
    hre_free(re);
    return NULL;
  }
  return re;
}

void hre_free(hregex *re) {
  if (re == NULL)
    return;
  /* a class bitmap may be shared by several I_CLASS copies (made by {n}
   * expansion); free each unique pointer once, nulling the aliases. */
  for (int i = 0; i < re->n; i++)
    if (re->p[i].op == I_CLASS && re->p[i].cls) {
      unsigned char *c = re->p[i].cls;
      free(c);
      for (int j = i; j < re->n; j++)
        if (re->p[j].op == I_CLASS && re->p[j].cls == c)
          re->p[j].cls = NULL;
    }
  for (int g = 0; g < 64; g++)
    free(re->names[g]);
  free(re->p);
  free(re);
}

int hre_ngroups(const hregex *re) { return re->ngroup; }
const char *hre_group_name(const hregex *re, int g) {
  return (g >= 0 && g < 64) ? re->names[g] : NULL;
}

static bool word_at(const char *s, size_t len, long i) {
  if (i < 0 || (size_t)i >= len)
    return false;
  unsigned char c = (unsigned char)s[i];
  return c == '_' || isalnum(c);
}

/* recursive backtracking VM */
static bool vm(const struct hregex *re, int pc, const char *s, size_t len, size_t sp, int *cap,
               long *steps) {
  for (;;) {
    if (--*steps < 0)
      return false;
    inst *in = &re->p[pc];
    switch (in->op) {
    case I_CHAR:
      if (sp < len && (unsigned char)s[sp] == in->c) {
        sp++;
        pc++;
        continue;
      }
      return false;
    case I_ANY:
      if (sp < len && s[sp] != '\n') {
        sp++;
        pc++;
        continue;
      }
      return false;
    case I_CLASS:
      if (sp < len && (in->cls[(unsigned char)s[sp] >> 3] & (1u << ((unsigned char)s[sp] & 7)))) {
        sp++;
        pc++;
        continue;
      }
      return false;
    case I_BOL:
      if (sp == 0 || s[sp - 1] == '\n') {
        pc++;
        continue;
      }
      return false;
    case I_EOL:
      if (sp == len || s[sp] == '\n') {
        pc++;
        continue;
      }
      return false;
    case I_WB:
      if (word_at(s, len, (long)sp - 1) != word_at(s, len, (long)sp)) {
        pc++;
        continue;
      }
      return false;
    case I_NWB:
      if (word_at(s, len, (long)sp - 1) == word_at(s, len, (long)sp)) {
        pc++;
        continue;
      }
      return false;
    case I_JMP:
      pc = in->x;
      continue;
    case I_SPLIT:
      if (vm(re, in->x, s, len, sp, cap, steps))
        return true;
      pc = in->y;
      continue;
    case I_SAVE: {
      int slot = in->x;
      int old = cap[slot];
      cap[slot] = (int)sp;
      if (vm(re, pc + 1, s, len, sp, cap, steps))
        return true;
      cap[slot] = old;
      return false;
    }
    case I_MATCH:
      return true;
    }
    return false;
  }
}

int hre_match(const hregex *re, const char *s, size_t len, size_t from, int *cap) {
  int nslot = 2 * (re->ngroup + 1);
  for (size_t start = from; start <= len; start++) {
    for (int i = 0; i < nslot; i++)
      cap[i] = -1;
    long steps = 20000000;
    if (vm(re, 0, s, len, start, cap, &steps))
      return 1;
  }
  return 0;
}

/* build the Terraform result of a match: the whole match (no groups), a tuple
 * (all-unnamed groups), or an object (all-named groups); unmatched group=null. */
hcl2_value *hre_submatch_value(const hregex *re, const char *s, const int *cap, char *e,
                               size_t es) {
  int ng = hre_ngroups(re), named = 0;
  for (int g = 1; g <= ng; g++)
    if (hre_group_name(re, g))
      named++;
  if (ng == 0)
    return mkstr_n(s + cap[0], (size_t)(cap[1] - cap[0]));
  if (named != 0 && named != ng) {
    everr(e, es, "regex: cannot mix named and unnamed capture groups");
    return NULL;
  }
  hcl2_value *out = named ? hcl2_object() : hcl2_tuple();
  if (out == NULL)
    return NULL;
  for (int g = 1; g <= ng; g++) {
    hcl2_value *gv = (cap[2 * g] < 0 || cap[2 * g + 1] < 0)
                         ? hcl2_null()
                         : mkstr_n(s + cap[2 * g], (size_t)(cap[2 * g + 1] - cap[2 * g]));
    bool ok = gv != NULL &&
              (named ? hcl2_object_set(out, hre_group_name(re, g), gv) : hcl2_tuple_push(out, gv));
    if (!ok) {
      hcl2_value_free(gv);
      hcl2_value_free(out);
      return NULL;
    }
  }
  return out;
}

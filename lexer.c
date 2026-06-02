#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * Lexer
 * ===========================================================================*/
/* enum tok / struct lexer live in hcl2_internal.h. */

void lx_err(struct lexer *l, const char *m) {
  if (l->err && l->errsz && l->err[0] == '\0')
    snprintf(l->err, l->errsz, "hcl2: %s", m);
}
static bool settext(struct lexer *l, const char *s, size_t n) {
  char *t = realloc(l->text, n + 1);
  if (t == NULL)
    return false;
  l->text = t;
  memcpy(t, s, n);
  t[n] = '\0';
  l->tlen = n;
  return true;
}
static bool id_start(int c) { return isalpha(c) || c == '_'; }
static bool id_char(int c) { return isalnum(c) || c == '_' || c == '-'; }

void lex(struct lexer *l) {
  while (l->p < l->end && isspace((unsigned char)*l->p))
    l->p++;
  if (l->p >= l->end) {
    l->tok = T_EOF;
    return;
  }
  char c = *l->p;
  switch (c) {
  case '(':
    l->p++;
    l->tok = T_LP;
    return;
  case ')':
    l->p++;
    l->tok = T_RP;
    return;
  case '[':
    l->p++;
    l->tok = T_LB;
    return;
  case ']':
    l->p++;
    l->tok = T_RB;
    return;
  case '{':
    l->p++;
    l->tok = T_LC;
    return;
  case '}':
    l->p++;
    l->tok = T_RC;
    return;
  case ',':
    l->p++;
    l->tok = T_COMMA;
    return;
  case '.':
    l->p++;
    l->tok = T_DOT;
    return;
  case ':':
    l->p++;
    l->tok = T_COLON;
    return;
  case '?':
    l->p++;
    l->tok = T_QUEST;
    return;
  case '+':
    l->p++;
    l->tok = T_PLUS;
    return;
  case '-':
    l->p++;
    l->tok = T_MINUS;
    return;
  case '*':
    l->p++;
    l->tok = T_STAR;
    return;
  case '/':
    l->p++;
    l->tok = T_SLASH;
    return;
  case '%':
    l->p++;
    l->tok = T_PCT;
    return;
  case '=':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_EQ;
      return;
    }
    l->p++;
    l->tok = T_ASSIGN;
    return;
  case '!':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_NE;
      return;
    }
    l->p++;
    l->tok = T_NOT;
    return;
  case '<':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_LE;
      return;
    }
    l->p++;
    l->tok = T_LT;
    return;
  case '>':
    if (l->end - l->p >= 2 && l->p[1] == '=') {
      l->p += 2;
      l->tok = T_GE;
      return;
    }
    l->p++;
    l->tok = T_GT;
    return;
  case '&':
    if (l->end - l->p >= 2 && l->p[1] == '&') {
      l->p += 2;
      l->tok = T_AND;
      return;
    }
    l->tok = T_ERR;
    lx_err(l, "unexpected '&'");
    return;
  case '|':
    if (l->end - l->p >= 2 && l->p[1] == '|') {
      l->p += 2;
      l->tok = T_OR;
      return;
    }
    l->tok = T_ERR;
    lx_err(l, "unexpected '|'");
    return;
  }
  if (c == '"') {
    /* capture raw inner bytes (escapes kept raw), stop at unescaped quote */
    const char *start = ++l->p;
    while (l->p < l->end && *l->p != '"') {
      if (*l->p == '\\' && l->p + 1 < l->end)
        l->p++;
      l->p++;
    }
    if (l->p >= l->end) {
      l->tok = T_ERR;
      lx_err(l, "unterminated string");
      return;
    }
    if (!settext(l, start, (size_t)(l->p - start))) {
      l->tok = T_ERR;
      return;
    }
    l->p++; /* closing quote */
    l->tok = T_STR;
    return;
  }
  if (isdigit((unsigned char)c)) {
    const char *start = l->p;
    while (l->p < l->end && (isdigit((unsigned char)*l->p) || *l->p == '.' || *l->p == 'e' ||
                             *l->p == 'E' || *l->p == '+' || *l->p == '-')) {
      /* allow exponent sign only right after e/E */
      if ((*l->p == '+' || *l->p == '-') && !(l->p > start && (l->p[-1] == 'e' || l->p[-1] == 'E')))
        break;
      l->p++;
    }
    if (!settext(l, start, (size_t)(l->p - start))) {
      l->tok = T_ERR;
      return;
    }
    l->tok = T_NUM;
    return;
  }
  if (id_start((unsigned char)c)) {
    const char *start = l->p;
    while (l->p < l->end && id_char((unsigned char)*l->p))
      l->p++;
    if (!settext(l, start, (size_t)(l->p - start))) {
      l->tok = T_ERR;
      return;
    }
    l->tok = T_IDENT;
    return;
  }
  l->tok = T_ERR;
  lx_err(l, "invalid character");
}

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * Lexer
 * ===========================================================================*/
/* enum tok / struct lexer live in hcl2_internal.h. */

void lx_linecol(const struct lexer *l, const char *pos, int *line, int *col) {
  int ln = 1;
  const char *bol = l->start; /* beginning of the line containing pos */
  for (const char *q = l->start; q != NULL && q < pos; q++)
    if (*q == '\n') {
      ln++;
      bol = q + 1;
    }
  *line = ln;
  *col = (l->start != NULL && pos != NULL) ? (int)(pos - bol) + 1 : 0;
}

void lx_err(struct lexer *l, const char *m) {
  if (!(l->err && l->errsz && l->err[0] == '\0'))
    return;
  if (l->start != NULL && l->tokpos != NULL) {
    int line, col;
    lx_linecol(l, l->tokpos, &line, &col);
    snprintf(l->err, l->errsz, "hcl2: %s at line %d, column %d", m, line, col);
  } else {
    snprintf(l->err, l->errsz, "hcl2: %s", m);
  }
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

/* Lex a heredoc body. The caller has consumed `<<` (and the optional `-`); the
 * cursor is at the delimiter word. The decoded body is stored in l->text and
 * the token set to T_HEREDOC. For `<<-`, the smallest run of leading spaces/
 * tabs common to all non-blank lines is removed from every line. The body is a
 * template (it still honours `${ }` interpolation at eval time) but, unlike a
 * quoted string, backslash escapes are kept literal. */
static void lex_heredoc(struct lexer *l, bool indented) {
  const char *ds = l->p;
  while (l->p < l->end && (isalnum((unsigned char)*l->p) || *l->p == '_'))
    l->p++;
  size_t dlen = (size_t)(l->p - ds);
  char delim[256];
  if (dlen == 0 || dlen >= sizeof(delim)) {
    l->tok = T_ERR;
    lx_err(l, dlen ? "heredoc delimiter too long" : "missing heredoc delimiter");
    return;
  }
  memcpy(delim, ds, dlen);
  delim[dlen] = '\0';
  while (l->p < l->end && (*l->p == ' ' || *l->p == '\t' || *l->p == '\r'))
    l->p++;
  if (l->p >= l->end || *l->p != '\n') {
    l->tok = T_ERR;
    lx_err(l, "expected newline after heredoc delimiter");
    return;
  }
  l->p++; /* consume newline; body starts here */
  const char *body = l->p, *q = l->p, *term = NULL;
  for (;;) {
    const char *ls = q, *t = ls;
    if (indented)
      while (t < l->end && (*t == ' ' || *t == '\t'))
        t++;
    if ((size_t)(l->end - t) >= dlen && memcmp(t, delim, dlen) == 0) {
      const char *r = t + dlen;
      while (r < l->end && (*r == ' ' || *r == '\t' || *r == '\r'))
        r++;
      if (r >= l->end || *r == '\n') { /* terminator line */
        term = ls;
        l->p = (r < l->end) ? r + 1 : r;
        break;
      }
    }
    while (q < l->end && *q != '\n')
      q++;
    if (q >= l->end) {
      l->tok = T_ERR;
      lx_err(l, "unterminated heredoc");
      return;
    }
    q++; /* next line */
  }
  size_t rawlen = (size_t)(term - body);
  if (!indented) {
    l->tok = settext(l, body, rawlen) ? T_HEREDOC : T_ERR;
    return;
  }
  /* compute common indentation over non-blank lines */
  size_t minind = (size_t)-1;
  for (const char *p = body; p < term;) {
    const char *le = p;
    while (le < term && *le != '\n')
      le++;
    const char *s = p;
    while (s < le && (*s == ' ' || *s == '\t'))
      s++;
    if (s < le && (size_t)(s - p) < minind)
      minind = (size_t)(s - p);
    p = (le < term) ? le + 1 : le;
  }
  if (minind == (size_t)-1)
    minind = 0;
  char *out = malloc(rawlen + 1);
  if (out == NULL) {
    l->tok = T_ERR;
    return;
  }
  char *w = out;
  for (const char *p = body; p < term;) {
    const char *le = p;
    while (le < term && *le != '\n')
      le++;
    const char *s = p;
    size_t drop = 0;
    while (s < le && drop < minind && (*s == ' ' || *s == '\t')) {
      s++;
      drop++;
    }
    memcpy(w, s, (size_t)(le - s));
    w += le - s;
    if (le < term) {
      *w++ = '\n';
      p = le + 1;
    } else {
      p = le;
    }
  }
  l->tok = settext(l, out, (size_t)(w - out)) ? T_HEREDOC : T_ERR;
  free(out);
}

void lex(struct lexer *l) {
  /* Skip whitespace and comments between tokens: '#' and '//' line comments,
     and '/' '*' ... '*' '/' block comments. */
  for (;;) {
    while (l->p < l->end && isspace((unsigned char)*l->p))
      l->p++;
    if (l->p < l->end && *l->p == '#') {
      while (l->p < l->end && *l->p != '\n')
        l->p++;
      continue;
    }
    if (l->end - l->p >= 2 && l->p[0] == '/' && l->p[1] == '/') {
      while (l->p < l->end && *l->p != '\n')
        l->p++;
      continue;
    }
    if (l->end - l->p >= 2 && l->p[0] == '/' && l->p[1] == '*') {
      l->p += 2;
      while (l->end - l->p >= 2 && !(l->p[0] == '*' && l->p[1] == '/'))
        l->p++;
      if (l->end - l->p >= 2)
        l->p += 2; /* consume the closing delimiter */
      continue;
    }
    break;
  }
  l->tokpos = l->p; /* token starts here, for error positions */
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
    if (l->end - l->p >= 3 && l->p[1] == '.' && l->p[2] == '.') {
      l->p += 3;
      l->tok = T_ELLIPSIS;
      return;
    }
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
    if (l->end - l->p >= 2 && l->p[1] == '>') {
      l->p += 2;
      l->tok = T_FATARROW;
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
    if (l->end - l->p >= 2 && l->p[1] == '<') {
      l->p += 2;
      bool indented = false;
      if (l->p < l->end && *l->p == '-') {
        indented = true;
        l->p++;
      }
      lex_heredoc(l, indented);
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
      /* a '.' followed by another '.' is never part of a number -- it begins a
         `...` spread, e.g. `xs[5]...` or `5...` */
      if (*l->p == '.' && l->p + 1 < l->end && l->p[1] == '.')
        break;
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

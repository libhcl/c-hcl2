#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* Filesystem helpers: lexical path cleaning (filepath.Clean semantics), whole
 * file reading and UTF-8 validation. */

/* Lexical path clean, a direct port of Go's path/filepath Clean. */
char *clean_path(const char *path) {
  size_t n = strlen(path);
  if (n == 0)
    return strdup(".");
  bool rooted = path[0] == '/';
  char *out = malloc(n + 2);
  if (out == NULL)
    return NULL;
  size_t w = 0, r = 0, dotdot = 0;
  if (rooted) {
    out[w++] = '/';
    r = 1;
    dotdot = 1;
  }
  while (r < n) {
    if (path[r] == '/') {
      r++;
    } else if (path[r] == '.' && (r + 1 == n || path[r + 1] == '/')) {
      r++;
    } else if (path[r] == '.' && path[r + 1] == '.' && (r + 2 == n || path[r + 2] == '/')) {
      r += 2;
      if (w > dotdot) {
        w--;
        while (w > dotdot && out[w] != '/')
          w--;
      } else if (!rooted) {
        if (w > 0)
          out[w++] = '/';
        out[w++] = '.';
        out[w++] = '.';
        dotdot = w;
      }
    } else {
      if ((rooted && w != 1) || (!rooted && w != 0))
        out[w++] = '/';
      while (r < n && path[r] != '/')
        out[w++] = path[r++];
    }
  }
  if (w == 0)
    out[w++] = '.';
  out[w] = '\0';
  return out;
}

char *read_file(const char *path, size_t *len, char *err, size_t es) {
  struct stat st;
  if (stat(path, &st) != 0) {
    everr(err, es, "file: no such file");
    return NULL;
  }
  if (S_ISDIR(st.st_mode)) {
    everr(err, es, "file: path is a directory");
    return NULL;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    everr(err, es, "file: cannot open file");
    return NULL;
  }
  size_t cap = (size_t)st.st_size + 1, used = 0;
  char *buf = malloc(cap);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  for (;;) {
    if (used + 1 >= cap) {
      cap *= 2;
      char *nb = realloc(buf, cap);
      if (nb == NULL) {
        free(buf);
        fclose(f);
        return NULL;
      }
      buf = nb;
    }
    size_t got = fread(buf + used, 1, cap - used - 1, f);
    used += got;
    if (got == 0)
      break;
  }
  if (ferror(f)) {
    free(buf);
    fclose(f);
    everr(err, es, "file: read error");
    return NULL;
  }
  fclose(f);
  buf[used] = '\0';
  *len = used;
  return buf;
}

/* match a single bracket class [..] at p against character c; on success sets
 * *adv to the number of pattern bytes consumed (including the closing ]). */
static bool class_match(const char *p, char c, size_t *adv) {
  const char *q = p + 1; /* past '[' */
  bool neg = false;
  if (*q == '^' || *q == '!') {
    neg = true;
    q++;
  }
  bool matched = false;
  bool first = true;
  while (*q && (*q != ']' || first)) {
    first = false;
    if (q[1] == '-' && q[2] != '\0' && q[2] != ']') {
      if ((unsigned char)c >= (unsigned char)q[0] && (unsigned char)c <= (unsigned char)q[2])
        matched = true;
      q += 3;
    } else {
      if (c == q[0])
        matched = true;
      q++;
    }
  }
  if (*q != ']')
    return false; /* malformed: no closing bracket */
  *adv = (size_t)(q - p) + 1;
  return matched != neg;
}

/* doublestar-style glob match over '/'-separated paths:
 *   *        any run of non-'/' characters
 *   ?        a single non-'/' character
 *   [..]     a character class within a segment
 *   **       any number of path segments (including zero), crossing '/'
 *   {a,b}    alternation: any of the comma-separated alternatives (nestable)
 */
bool glob_match(const char *p, const char *s) {
  while (*p) {
    if (*p == '{') {
      /* find the matching '}' (tracking nested braces and bracket classes) */
      const char *close = NULL;
      int depth = 0;
      bool incls = false;
      for (const char *r = p + 1; *r; r++) {
        if (incls) {
          if (*r == ']')
            incls = false;
        } else if (*r == '[')
          incls = true;
        else if (*r == '{')
          depth++;
        else if (*r == '}') {
          if (depth == 0) {
            close = r;
            break;
          }
          depth--;
        }
      }
      if (close == NULL) { /* unbalanced: treat '{' literally */
        if (*p != *s)
          return false;
        p++;
        s++;
        continue;
      }
      /* try each depth-0 alternative as (alternative + tail) against s */
      size_t taillen = strlen(close + 1);
      const char *as = p + 1;
      depth = 0;
      incls = false;
      for (const char *r = p + 1; r <= close; r++) {
        bool boundary = (r == close);
        if (!boundary) {
          if (incls) {
            if (*r == ']')
              incls = false;
          } else if (*r == '[')
            incls = true;
          else if (*r == '{')
            depth++;
          else if (*r == '}')
            depth--;
          else if (*r == ',' && depth == 0)
            boundary = true;
        }
        if (boundary) {
          size_t altlen = (size_t)(r - as);
          char *tmp = malloc(altlen + taillen + 1);
          if (tmp == NULL)
            return false;
          memcpy(tmp, as, altlen);
          memcpy(tmp + altlen, close + 1, taillen + 1);
          bool ok = glob_match(tmp, s);
          free(tmp);
          if (ok)
            return true;
          as = r + 1;
        }
      }
      return false;
    } else if (p[0] == '*' && p[1] == '*') {
      const char *pp = p + 2;
      if (*pp == '/')
        pp++;
      if (*pp == '\0')
        return true; /* trailing ** matches the rest, including '/' */
      for (const char *t = s;; t++) {
        if (glob_match(pp, t))
          return true;
        if (*t == '\0')
          return false;
      }
    } else if (*p == '*') {
      p++;
      for (const char *t = s;; t++) {
        if (glob_match(p, t))
          return true;
        if (*t == '\0' || *t == '/')
          return false;
      }
    } else if (*p == '?') {
      if (*s == '\0' || *s == '/')
        return false;
      p++;
      s++;
    } else if (*p == '[') {
      size_t adv;
      if (*s == '\0' || *s == '/' || !class_match(p, *s, &adv))
        return false;
      p += adv;
      s++;
    } else {
      if (*p != *s)
        return false;
      p++;
      s++;
    }
  }
  return *s == '\0';
}

bool utf8_valid(const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    size_t extra;
    unsigned cp;
    if (c < 0x80) {
      i++;
      continue;
    } else if ((c & 0xe0) == 0xc0) {
      extra = 1;
      cp = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
      extra = 2;
      cp = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
      extra = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= n)
      return false;
    for (size_t k = 1; k <= extra; k++) {
      unsigned char cc = (unsigned char)s[i + k];
      if ((cc & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (cc & 0x3f);
    }
    /* reject overlong encodings, surrogates and out-of-range code points */
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
        (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
      return false;
    i += extra + 1;
  }
  return true;
}

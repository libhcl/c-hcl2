#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

/* ===========================================================================
 * JSON profile (M5) -- value layer
 *
 * Parse a JSON document into the cty-lite value model: object -> object,
 * array -> tuple, string -> string (literal; NOT yet interpreted as an HCL
 * template), number -> number, true/false -> bool, null -> null.
 *
 * This is the value mapping of HCL's JSON profile. The full *body* profile
 * (schema-driven attribute-vs-block resolution and string-as-template
 * decoding) is future work; see ROADMAP.md.
 * ===========================================================================*/

struct jp {
  const char *p, *end;
  char *err;
  size_t errsz;
  hcl2_ctx *ctx; /* template-eval profile only (hcl2_json_eval) */
  bool tmpl;     /* when true, JSON strings are evaluated as HCL templates */
};

static void jerr(struct jp *j, const char *m) {
  if (j->err != NULL && j->errsz > 0 && j->err[0] == '\0')
    snprintf(j->err, j->errsz, "hcl2: json: %s", m);
}
static void jws(struct jp *j) {
  while (j->p < j->end && isspace((unsigned char)*j->p))
    j->p++;
}
static bool numch(char c) {
  return isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static hcl2_value *jvalue(struct jp *j);

static void utf8_encode(uint32_t cp, char **w) {
  if (cp < 0x80) {
    *(*w)++ = (char)cp;
  } else if (cp < 0x800) {
    *(*w)++ = (char)(0xC0 | (cp >> 6));
    *(*w)++ = (char)(0x80 | (cp & 0x3F));
  } else {
    *(*w)++ = (char)(0xE0 | (cp >> 12));
    *(*w)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *(*w)++ = (char)(0x80 | (cp & 0x3F));
  }
}

/* Parse a JSON string at the opening quote into a malloc'd C string, or NULL. */
static char *jstring_raw(struct jp *j) {
  if (j->p >= j->end || *j->p != '"') {
    jerr(j, "expected a string");
    return NULL;
  }
  j->p++;
  char *out = malloc((size_t)(j->end - j->p) + 1); /* decoding never grows */
  if (out == NULL)
    return NULL;
  char *w = out;
  while (j->p < j->end && *j->p != '"') {
    char ch = *j->p++;
    if (ch != '\\') {
      *w++ = ch;
      continue;
    }
    if (j->p >= j->end)
      goto bad;
    char e = *j->p++;
    switch (e) {
    case '"':
      *w++ = '"';
      break;
    case '\\':
      *w++ = '\\';
      break;
    case '/':
      *w++ = '/';
      break;
    case 'b':
      *w++ = '\b';
      break;
    case 'f':
      *w++ = '\f';
      break;
    case 'n':
      *w++ = '\n';
      break;
    case 'r':
      *w++ = '\r';
      break;
    case 't':
      *w++ = '\t';
      break;
    case 'u': {
      if (j->end - j->p < 4)
        goto bad;
      uint32_t cp = 0;
      for (int i = 0; i < 4; i++) {
        char h = *j->p++;
        cp <<= 4;
        if (h >= '0' && h <= '9')
          cp |= (uint32_t)(h - '0');
        else if (h >= 'a' && h <= 'f')
          cp |= (uint32_t)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F')
          cp |= (uint32_t)(h - 'A' + 10);
        else
          goto bad;
      }
      utf8_encode(cp, &w);
      break;
    }
    default:
      goto bad;
    }
  }
  if (j->p >= j->end || *j->p != '"')
    goto bad;
  j->p++; /* closing quote */
  *w = '\0';
  return out;
bad:
  jerr(j, "invalid string");
  free(out);
  return NULL;
}

static hcl2_value *jstring(struct jp *j) {
  char *s = jstring_raw(j);
  if (s == NULL)
    return NULL;
  /* JSON profile: a string is a literal. JSON-eval profile: it is an HCL
   * template (heredoc mode -- JSON already un-escaped, so backslashes stay
   * literal and only ${ } / %{ } are interpreted). */
  hcl2_value *v = j->tmpl ? eval_template(s, true, j->ctx, j->err, j->errsz) : hcl2_string(s);
  free(s);
  return v;
}

static hcl2_value *jnumber(struct jp *j) {
  const char *s = j->p;
  while (j->p < j->end && numch(*j->p))
    j->p++;
  size_t n = (size_t)(j->p - s);
  char buf[64];
  if (n == 0 || n >= sizeof(buf)) {
    jerr(j, "invalid number");
    return NULL;
  }
  memcpy(buf, s, n);
  buf[n] = '\0';
  char *endp = NULL;
  double d = strtod(buf, &endp);
  if (endp != buf + n) {
    jerr(j, "invalid number");
    return NULL;
  }
  return hcl2_number(d);
}

static hcl2_value *jlit(struct jp *j, const char *lit, hcl2_value *v) {
  size_t n = strlen(lit);
  if ((size_t)(j->end - j->p) < n || memcmp(j->p, lit, n) != 0) {
    hcl2_value_free(v);
    jerr(j, "invalid literal");
    return NULL;
  }
  j->p += n;
  return v;
}

static hcl2_value *jarray(struct jp *j) {
  j->p++; /* '[' */
  hcl2_value *t = hcl2_tuple();
  if (t == NULL)
    return NULL;
  jws(j);
  if (j->p < j->end && *j->p == ']') {
    j->p++;
    return t;
  }
  for (;;) {
    hcl2_value *e = jvalue(j);
    if (e == NULL || !hcl2_tuple_push(t, e)) {
      hcl2_value_free(e);
      hcl2_value_free(t);
      return NULL;
    }
    jws(j);
    if (j->p < j->end && *j->p == ',') {
      j->p++;
      continue;
    }
    if (j->p < j->end && *j->p == ']') {
      j->p++;
      return t;
    }
    jerr(j, "expected ',' or ']' in array");
    hcl2_value_free(t);
    return NULL;
  }
}

static hcl2_value *jobject(struct jp *j) {
  j->p++; /* '{' */
  hcl2_value *o = hcl2_object();
  if (o == NULL)
    return NULL;
  jws(j);
  if (j->p < j->end && *j->p == '}') {
    j->p++;
    return o;
  }
  for (;;) {
    jws(j);
    char *key = jstring_raw(j);
    if (key == NULL) {
      hcl2_value_free(o);
      return NULL;
    }
    jws(j);
    if (j->p >= j->end || *j->p != ':') {
      free(key);
      hcl2_value_free(o);
      jerr(j, "expected ':' after object key");
      return NULL;
    }
    j->p++;
    hcl2_value *val = jvalue(j);
    if (val == NULL || !hcl2_object_set(o, key, val)) {
      free(key);
      hcl2_value_free(val);
      hcl2_value_free(o);
      return NULL;
    }
    free(key);
    jws(j);
    if (j->p < j->end && *j->p == ',') {
      j->p++;
      continue;
    }
    if (j->p < j->end && *j->p == '}') {
      j->p++;
      return o;
    }
    jerr(j, "expected ',' or '}' in object");
    hcl2_value_free(o);
    return NULL;
  }
}

static hcl2_value *jvalue(struct jp *j) {
  jws(j);
  if (j->p >= j->end) {
    jerr(j, "unexpected end of input");
    return NULL;
  }
  char c = *j->p;
  switch (c) {
  case '{':
    return jobject(j);
  case '[':
    return jarray(j);
  case '"':
    return jstring(j);
  case 't':
    return jlit(j, "true", hcl2_bool(true));
  case 'f':
    return jlit(j, "false", hcl2_bool(false));
  case 'n':
    return jlit(j, "null", hcl2_null());
  default:
    if (c == '-' || isdigit((unsigned char)c))
      return jnumber(j);
    jerr(j, "unexpected character");
    return NULL;
  }
}

static hcl2_value *json_run(struct jp *j) {
  hcl2_value *v = jvalue(j);
  if (v == NULL)
    return NULL;
  jws(j);
  if (j->p != j->end) {
    jerr(j, "trailing content after JSON value");
    hcl2_value_free(v);
    return NULL;
  }
  return v;
}

hcl2_value *hcl2_parse_json(const char *src, size_t len, char *err, size_t errsz) {
  if (err != NULL && errsz > 0)
    err[0] = '\0';
  struct jp j = {.p = src, .end = src + len, .err = err, .errsz = errsz};
  return json_run(&j);
}

hcl2_value *hcl2_json_eval(const char *src, size_t len, hcl2_ctx *ctx, char *err, size_t errsz) {
  if (err != NULL && errsz > 0)
    err[0] = '\0';
  struct jp j = {.p = src, .end = src + len, .err = err, .errsz = errsz, .ctx = ctx, .tmpl = true};
  return json_run(&j);
}

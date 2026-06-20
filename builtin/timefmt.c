#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* RFC 3339 timestamps, Go-style durations and Terraform's formatdate tokens.
 * Times are reduced to a UTC epoch (seconds) plus the original zone offset so
 * the same value formats back in the timezone it was given in. */

static long long days_from_civil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

static void civil_from_days(long long z, long long *y, unsigned *m, unsigned *d) {
  z += 719468;
  long long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long long yy = (long long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : (unsigned)-9);
  *y = yy + (*m <= 2);
}

static long long fdiv(long long a, long long b) {
  long long q = a / b, r = a % b;
  if (r != 0 && ((r < 0) != (b < 0)))
    q--;
  return q;
}

bool rfc3339_parse(const char *s, long long *epoch, int *off, char *err, size_t es) {
  int Y, Mo, D, H, Mi, S, nn = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d%n", &Y, &Mo, &D, &H, &Mi, &S, &nn) != 6) {
    everr(err, es, "timestamp: not an RFC 3339 timestamp");
    return false;
  }
  if (Mo < 1 || Mo > 12 || D < 1 || D > 31 || H > 23 || Mi > 59 || S > 60) {
    everr(err, es, "timestamp: field out of range");
    return false;
  }
  const char *p = s + nn;
  if (*p == '.') {
    p++;
    if (!isdigit((unsigned char)*p)) {
      everr(err, es, "timestamp: malformed fractional seconds");
      return false;
    }
    while (isdigit((unsigned char)*p))
      p++;
  }
  int o = 0;
  if (*p == 'Z' || *p == 'z') {
    p++;
  } else if (*p == '+' || *p == '-') {
    int sign = (*p == '-') ? -1 : 1, oh, om, on = 0;
    if (sscanf(p + 1, "%2d:%2d%n", &oh, &om, &on) != 2 || on != 5 || oh > 23 || om > 59) {
      everr(err, es, "timestamp: malformed timezone offset");
      return false;
    }
    o = sign * (oh * 3600 + om * 60);
    p += 6;
  } else {
    everr(err, es, "timestamp: missing timezone designator");
    return false;
  }
  if (*p != '\0') {
    everr(err, es, "timestamp: trailing characters");
    return false;
  }
  *epoch = days_from_civil(Y, (unsigned)Mo, (unsigned)D) * 86400 + H * 3600 + Mi * 60 + S - o;
  *off = o;
  return true;
}

char *rfc3339_format(long long epoch, int off) {
  long long local = epoch + off;
  long long days = fdiv(local, 86400);
  int rem = (int)(local - days * 86400);
  long long Y;
  unsigned M, D;
  civil_from_days(days, &Y, &M, &D);
  char zone[8];
  if (off == 0)
    strcpy(zone, "Z");
  else {
    int a = off < 0 ? -off : off;
    snprintf(zone, sizeof zone, "%c%02d:%02d", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
  }
  char buf[48];
  snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02d%s", Y, M, D, rem / 3600,
           (rem % 3600) / 60, rem % 60, zone);
  return strdup(buf);
}

bool duration_parse(const char *s, long long *secs, char *err, size_t es) {
  long long sign = 1;
  const char *p = s;
  if (*p == '+' || *p == '-') {
    if (*p == '-')
      sign = -1;
    p++;
  }
  if (*p == '\0') {
    everr(err, es, "timeadd: empty duration");
    return false;
  }
  if (strcmp(p, "0") == 0) {
    *secs = 0;
    return true;
  }
  double total = 0;
  while (*p) {
    char *end;
    double v = strtod(p, &end);
    if (end == p) {
      everr(err, es, "timeadd: invalid duration");
      return false;
    }
    p = end;
    double unit;
    if (strncmp(p, "ns", 2) == 0)
      unit = 1e-9, p += 2;
    else if (strncmp(p, "us", 2) == 0)
      unit = 1e-6, p += 2;
    else if (strncmp(p, "\xc2\xb5s", 3) == 0)
      unit = 1e-6, p += 3;
    else if (strncmp(p, "ms", 2) == 0)
      unit = 1e-3, p += 2;
    else if (*p == 's')
      unit = 1, p += 1;
    else if (*p == 'm')
      unit = 60, p += 1;
    else if (*p == 'h')
      unit = 3600, p += 1;
    else {
      everr(err, es, "timeadd: unknown duration unit");
      return false;
    }
    total += v * unit;
  }
  double t = sign * total;
  if (t != (double)(long long)t) {
    everr(err, es, "timeadd: sub-second precision not supported");
    return false;
  }
  *secs = (long long)t;
  return true;
}

char *format_date(const char *fmt, long long epoch, int off, char *err, size_t es) {
  static const char *mon_full[] = {"January",   "February", "March",    "April",
                                   "May",       "June",     "July",     "August",
                                   "September", "October",  "November", "December"};
  static const char *mon_abbr[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const char *wd_full[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
  static const char *wd_abbr[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

  long long local = epoch + off;
  long long days = fdiv(local, 86400);
  int rem = (int)(local - days * 86400);
  long long Y;
  unsigned M, D;
  civil_from_days(days, &Y, &M, &D);
  int H = rem / 3600, Mi = (rem % 3600) / 60, S = rem % 60;
  int wd = (int)(((days % 7) + 4) % 7);
  if (wd < 0)
    wd += 7;
  int h12 = H % 12;
  if (h12 == 0)
    h12 = 12;

  struct sb out = {0};
  const char *p = fmt;
  while (*p) {
    if (*p == '\'') { /* '' = literal quote, otherwise quoted span */
      p++;
      if (*p == '\'') {
        sb_put(&out, "'", 1);
        p++;
        continue;
      }
      while (*p && *p != '\'')
        sb_put(&out, p++, 1);
      if (*p == '\'')
        p++;
      continue;
    }
    if (!isalpha((unsigned char)*p)) {
      sb_put(&out, p++, 1);
      continue;
    }
    char c = *p;
    int run = 0;
    while (p[run] == c)
      run++;
    char tmp[16];
    const char *emit = NULL;
    switch (c) {
    case 'Y':
      if (run == 4)
        snprintf(tmp, sizeof tmp, "%04lld", Y), emit = tmp;
      else if (run == 2)
        snprintf(tmp, sizeof tmp, "%02lld", Y % 100), emit = tmp;
      break;
    case 'M':
      if (run == 4)
        emit = mon_full[M - 1];
      else if (run == 3)
        emit = mon_abbr[M - 1];
      else if (run == 2)
        snprintf(tmp, sizeof tmp, "%02u", M), emit = tmp;
      else if (run == 1)
        snprintf(tmp, sizeof tmp, "%u", M), emit = tmp;
      break;
    case 'D':
      if (run == 2)
        snprintf(tmp, sizeof tmp, "%02u", D), emit = tmp;
      else if (run == 1)
        snprintf(tmp, sizeof tmp, "%u", D), emit = tmp;
      break;
    case 'E':
      if (run == 4)
        emit = wd_full[wd];
      else if (run == 3)
        emit = wd_abbr[wd];
      break;
    case 'h': /* 24-hour */
      if (run == 2)
        snprintf(tmp, sizeof tmp, "%02d", H), emit = tmp;
      else if (run == 1)
        snprintf(tmp, sizeof tmp, "%d", H), emit = tmp;
      break;
    case 'H': /* 12-hour */
      if (run == 2)
        snprintf(tmp, sizeof tmp, "%02d", h12), emit = tmp;
      else if (run == 1)
        snprintf(tmp, sizeof tmp, "%d", h12), emit = tmp;
      break;
    case 'A':
      if (run == 2)
        emit = (H < 12) ? "AM" : "PM";
      break;
    case 'a':
      if (run == 2)
        emit = (H < 12) ? "am" : "pm";
      break;
    case 'm':
      if (run == 2)
        snprintf(tmp, sizeof tmp, "%02d", Mi), emit = tmp;
      else if (run == 1)
        snprintf(tmp, sizeof tmp, "%d", Mi), emit = tmp;
      break;
    case 's':
      if (run == 2)
        snprintf(tmp, sizeof tmp, "%02d", S), emit = tmp;
      else if (run == 1)
        snprintf(tmp, sizeof tmp, "%d", S), emit = tmp;
      break;
    case 'Z': {
      int a = off < 0 ? -off : off;
      char sgn = off < 0 ? '-' : '+';
      if (run == 5)
        snprintf(tmp, sizeof tmp, "%c%02d%02d", sgn, a / 3600, (a % 3600) / 60), emit = tmp;
      else if (run == 4)
        snprintf(tmp, sizeof tmp, "%c%02d:%02d", sgn, a / 3600, (a % 3600) / 60), emit = tmp;
      else if (run == 3)
        emit = (off == 0) ? "UTC" : "";
      else if (run == 1) {
        if (off == 0)
          emit = "Z";
        else
          snprintf(tmp, sizeof tmp, "%c%02d:%02d", sgn, a / 3600, (a % 3600) / 60), emit = tmp;
      }
      break;
    }
    default:
      break;
    }
    if (emit == NULL) {
      everr(err, es, "formatdate: unsupported format verb");
      free(out.p);
      return NULL;
    }
    sb_puts(&out, emit);
    p += run;
  }
  if (out.oom) {
    free(out.p);
    return NULL;
  }
  if (out.p == NULL)
    return strdup("");
  return out.p; /* sb_put keeps the buffer NUL-terminated */
}

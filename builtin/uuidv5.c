#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hcl2.h"
#include "hcl2_alloc.h"
#include "hcl2_internal.h"

#include "builtin.h"

/* RFC 4122 predefined namespaces (differ only in byte 3). */
static const unsigned char NS[4][16] = {
    /* dns  */ {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4,
                0x30,                                                                                     0xc8},
    /* url  */
    {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30,
                0xc8                                                                                          },
    /* oid  */
    {0x6b, 0xa7, 0xb8, 0x12, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30,
                0xc8                                                                                          },
    /* x500 */
    {0x6b, 0xa7, 0xb8, 0x14, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30,
                0xc8                                                                                          },
};

static int hexnib(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* parse a 36-char "8-4-4-4-12" UUID into 16 bytes; returns false if invalid. */
static bool parse_uuid(const char *s, unsigned char out[16]) {
  int bi = 0;
  for (const char *p = s; *p; p++) {
    if (*p == '-')
      continue;
    int hi = hexnib(*p);
    if (hi < 0 || p[1] == '\0')
      return false;
    int lo = hexnib(p[1]);
    if (lo < 0)
      return false;
    if (bi >= 16)
      return false;
    out[bi++] = (unsigned char)((hi << 4) | lo);
    p++;
  }
  return bi == 16;
}

hcl2_value *bi_uuidv5(const hcl2_value *const *a, size_t n, char *e, size_t es) {
  if (n != 2 || a[0]->kind != HCL2_STRING || a[1]->kind != HCL2_STRING) {
    everr(e, es, "uuidv5() needs (namespace, name)");
    return NULL;
  }
  const char *ns = a[0]->str, *name = a[1]->str;
  unsigned char nsb[16];
  if (strcmp(ns, "dns") == 0)
    memcpy(nsb, NS[0], 16);
  else if (strcmp(ns, "url") == 0)
    memcpy(nsb, NS[1], 16);
  else if (strcmp(ns, "oid") == 0)
    memcpy(nsb, NS[2], 16);
  else if (strcmp(ns, "x500") == 0)
    memcpy(nsb, NS[3], 16);
  else if (!parse_uuid(ns, nsb)) {
    everr(e, es, "uuidv5() namespace must be dns/url/oid/x500 or a UUID");
    return NULL;
  }

  size_t nl = strlen(name);
  unsigned char *buf = malloc(16 + nl);
  if (buf == NULL)
    return NULL;
  memcpy(buf, nsb, 16);
  memcpy(buf + 16, name, nl);
  unsigned char h[20];
  hc_sha1(buf, 16 + nl, h);
  free(buf);

  h[6] = (unsigned char)((h[6] & 0x0f) | 0x50); /* version 5 */
  h[8] = (unsigned char)((h[8] & 0x3f) | 0x80); /* RFC 4122 variant */

  char out[37];
  snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13],
           h[14], h[15]);
  return hcl2_string(out);
}
